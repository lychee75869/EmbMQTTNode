#!/usr/bin/env python3
"""
Modbus TCP Slave Simulator
==========================
用于 EmbMQTTNode 开发调试，模拟一个 Modbus TCP 从设备。
无需真实 PLC/传感器硬件即可验证 Modbus 模块。

启动后监听 127.0.0.1:502，提供 4 个保持寄存器：
  40001 - 温度 × 10  (int16, 例 256 → 25.6℃)
  40002 - 湿度 × 10  (int16, 例 523 → 52.3%)
  40003 - 气压高字    (float32 高 16 位)
  40004 - 气压低字    (float32 低 16 位, 例 {40003,40004} → 1013.25 hPa)

安装依赖:
  pip install pymodbus

使用:
  python tools/modbus_slave_sim.py
  默认监听 127.0.0.1:502

  python tools/modbus_slave_sim.py --port 1502  # 自定义端口
"""

import struct
import random
import time
import argparse
from pymodbus.server import StartTcpServer
from pymodbus.datastore import ModbusSequentialDataBlock
from pymodbus.datastore import ModbusSlaveContext, ModbusServerContext


class SimSensor:
    """模拟传感器数据，带缓慢随机漂移"""

    def __init__(self):
        self.temp = 25.0   # ℃
        self.humid = 55.0  # %
        self.press = 1013.25  # hPa

    def drift(self):
        """每次调用时数据小幅随机波动，模拟真实传感器漂移"""
        self.temp  += random.gauss(0, 0.05)       # 标准差 0.05℃
        self.humid += random.gauss(0, 0.10)       # 标准差 0.10%
        self.press += random.gauss(0, 0.15)       # 标准差 0.15hPa

        # 边界约束
        self.temp  = max(10.0, min(45.0, self.temp))
        self.humid = max(20.0, min(95.0, self.humid))
        self.press = max(980.0, min(1050.0, self.press))

        return self.temp, self.humid, self.press

    def to_registers(self):
        """转为 Modbus 寄存器格式"""
        # 温度: int16 × 10
        temp_reg = int(self.temp * 10)
        if temp_reg < 0:
            temp_reg = temp_reg & 0xFFFF  # 补码

        # 湿度: int16 × 10
        humid_reg = int(self.humid * 10)
        if humid_reg < 0:
            humid_reg = humid_reg & 0xFFFF

        # 气压: float32 → 2 个 uint16（大端）
        press_bytes = struct.pack('>f', self.press)
        press_hi = (press_bytes[0] << 8) | press_bytes[1]
        press_lo = (press_bytes[2] << 8) | press_bytes[3]

        return [temp_reg, humid_reg, press_hi, press_lo]


class UpdatingDataBlock(ModbusSequentialDataBlock):
    """自定义 DataBlock：每次读取时自动更新传感器数据"""

    def __init__(self, sim):
        self.sim = sim
        super().__init__(0, [0] * 4)

    def getValues(self, address, count=1):
        """读取寄存器时自动生成最新数据"""
        self.sim.drift()
        regs = self.sim.to_registers()
        self.values = regs  # 更新内部值
        return super().getValues(address, count)


def main():
    parser = argparse.ArgumentParser(
        description="EmbMQTTNode Modbus TCP Slave Simulator"
    )
    parser.add_argument("--host", default="127.0.0.1",
                        help="监听地址 (默认 127.0.0.1)")
    parser.add_argument("--port", type=int, default=502,
                        help="监听端口 (默认 502)")
    parser.add_argument("--slave-id", type=int, default=1,
                        help="从站地址 (默认 1)")
    args = parser.parse_args()

    sim = SimSensor()

    # 构建从站数据存储
    # 保持寄存器: 地址 0 起始 → 对应 40001-40004
    hr_block = UpdatingDataBlock(sim)
    store = ModbusSlaveContext(
        di=None, co=None,
        hr=hr_block,   # Holding Registers (40001+)
        ir=None
    )
    context = ModbusServerContext(slaves={args.slave_id: store}, single=False)

    print(f"============================================")
    print(f" EmbMQTTNode Modbus TCP Slave Simulator")
    print(f"============================================")
    print(f" Listening:  {args.host}:{args.port}")
    print(f" Slave ID:   {args.slave_id}")
    print(f" Registers:  40001 = Temperature × 10 (int16)")
    print(f"             40002 = Humidity × 10 (int16)")
    print(f"             40003-40004 = Pressure (float32, 大端)")
    print(f"")
    print(f" EmbMQTTNode 配置示例:")
    print(f"   modbus_enabled = 1")
    print(f"   modbus_mode = tcp")
    print(f"   modbus_tcp_host = {args.host}")
    print(f"   modbus_tcp_port = {args.port}")
    print(f"   modbus_reg_1 = {args.slave_id},40001,1,3,int16,temperature,0.1,0")
    print(f"   modbus_reg_2 = {args.slave_id},40002,1,3,int16,humidity,0.1,0")
    print(f"   modbus_reg_3 = {args.slave_id},40003,2,3,float32,pressure,1.0,0")
    print(f"============================================")
    print(f" Press Ctrl+C to stop")
    print()

    StartTcpServer(context, address=(args.host, args.port))


if __name__ == "__main__":
    main()
