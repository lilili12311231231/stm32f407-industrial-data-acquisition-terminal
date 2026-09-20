# -*- coding: utf-8 -*-
"""
阶段5：监控大屏 —— 实时数据 + 历史曲线 + 报警阈值
运行：python3 app.py    浏览器打开 http://<树莓派IP>:5000
"""
import threading
import time
import minimalmodbus
from flask import Flask, render_template
from flask_socketio import SocketIO

app = Flask(__name__)
app.config['SECRET_KEY'] = 'iot-stage5'

# async_mode='threading'：线程模式，不依赖 eventlet/gevent，最稳（会降级为 long-polling，够用）
socketio = SocketIO(app, async_mode='threading')

# —— Modbus 配置（和 read_modbus.py 完全一样）——
SERIAL_PORT = '/dev/ttyUSB0'   # USB转485(CH340) 在树莓派上的串口
SLAVE_ADDR = 1                 # STM32 从站地址

# —— 报警阈值（演示用，方便手动触发：拨电位器/手摸芯片/遮光）——
THRESHOLDS = {
    'voltage_min': 2.0,       # 电压 < 2.0V  欠压报警
    'temperature_max': 40.0,  # 温度 > 40℃   高温报警
    'light_min': 20,          # 光强 < 20%   过暗报警
}


def make_instrument():
    """创建 Modbus 主站对象（相当于 C 里的「初始化串口 + 协议栈」）"""
    instr = minimalmodbus.Instrument(SERIAL_PORT, SLAVE_ADDR)
    instr.serial.baudrate = 9600
    instr.serial.bytesize = 8
    instr.serial.parity = minimalmodbus.serial.PARITY_NONE
    instr.serial.stopbits = 1
    instr.serial.timeout = 1
    return instr


@app.route('/')
def index():
    """浏览器访问 http://IP:5000 时，返回网页"""
    return render_template('index.html')


def modbus_loop():
    """后台线程：每秒读一次三路寄存器，判断报警，广播给所有浏览器"""
    instr = make_instrument()
    while True:
        try:
            values = instr.read_registers(0, 3, 3)  # 起始0, 读3个, 功能码03
            voltage = values[0] / 100.0      # 寄存器0 = 电压×100
            temperature = values[1] / 10.0   # 寄存器1 = 温度×10
            light = values[2]                # 寄存器2 = 光强%
            data = {
                'voltage': voltage,
                'temperature': temperature,
                'light': light,
                'alarm': {  # 关键：后端判断报警，前端只管显示
                    'voltage': voltage < THRESHOLDS['voltage_min'],
                    'temperature': temperature > THRESHOLDS['temperature_max'],
                    'light': light < THRESHOLDS['light_min'],
                },
            }
            socketio.emit('data', data)  # 关键：把数据「推送」出去
        except Exception as e:
            socketio.emit('error', {'message': str(e)})
        time.sleep(1)


if __name__ == '__main__':
    # 单独开一个线程跑读数据循环，主线程留给网页服务
    t = threading.Thread(target=modbus_loop, daemon=True)
    t.start()
    # 0.0.0.0 = 监听所有网卡，手机/电脑在局域网都能访问
    # allow_unsafe_werkzeug=True：flask-socketio 5.6+ 默认禁止用 Werkzeug 开发服务器，需显式声明（局域网演示够用）
    socketio.run(app, host='0.0.0.0', port=5000, allow_unsafe_werkzeug=True)
