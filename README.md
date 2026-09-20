# 基于 STM32F407 的工业数据采集终端

> 一款端到端的工业物联网数据采集系统：STM32 采集三路模拟量 → Modbus-RTU 从站 → RS485 差分总线 → 树莓派网关 → 网页实时监控大屏（支持外网访问）。

## 项目简介

以 STM32F407ZGT6（正点原子探索者）为核心，采集**电压 / 温度 / 光强**三路模拟量，经两级数字滤波后写入 Modbus 保持寄存器；通过 RS485 总线传给树莓派网关（Modbus 主站），再由 Flask + SocketIO 实时推送到浏览器监控大屏，实现「工业现场数据 → 云端可视化」的完整闭环。

## 系统架构

```
┌──────────────────────────── STM32F407（从站）────────────────────────────┐
│  ADC1（DMA 双通道）  电压 / 温度                                        │
│  ADC3（单通道轮询）   光强                                              │
│        ↓ 两级滤波（中值去尖峰 + 滑动平均平滑）                            │
│  FreeRTOS 多任务（采集 / 显示 / Modbus / 看门狗 / 日志）                  │
│        ↓ 写 mb_holding_regs[]                                          │
│  Modbus-RTU 从站（USART2，9600）                                        │
└──────────────────────────────────┬──────────────────────────────────────┘
                                   │ RS485（A/B 差分，SP3485）
                                   ▼
┌──────────────────────── 树莓派 3B+（主站 + 网关）────────────────────────┐
│  minimalmodbus 每秒轮询：01 03 00 00 00 03                              │
│  Flask + Flask-SocketIO（后台线程读数据 + 主动推送）                      │
└──────────────────────────────────┬──────────────────────────────────────┘
                                   │ SocketIO 实时推送
                                   ▼
                       ┌────────────────────────────┐
                       │        浏览器监控大屏        │
                       │  实时卡片 + 历史曲线 + 报警   │
                       │   （cpolar 内网穿透，外网访问）│
                       └────────────────────────────┘
```

## 功能特性

### 采集端（STM32 固件）
- **三路模拟量采集**：电压（电位器分压）、温度（芯片内部温度传感器）、光强（光敏电阻）
- **两级数字滤波**：中值滤波（窗口 5 去尖峰）+ 滑动平均（窗口 8 平滑）
- **FreeRTOS 多任务**：采集 / 显示 / Modbus / 看门狗 / 日志 任务并行，互斥锁保护共享数据
- **Modbus-RTU 从站**：功能码 03 读保持寄存器，寄存器 0/1/2 分别存电压×100、温度×10、光强 0~100
- **RS485 差分总线**：板载 SP3485，抗干扰，适配工业现场
- **独立看门狗 IWDG**：约 2.5s 超时，程序跑飞自动复位
- **日志库**：分级（DEBUG/INFO/WARN/ERROR）+ 阈值过滤 + SD 卡落盘（FATFS）+ 文件轮转
- **EEPROM 掉电保存**：AT24C02（I2C），断电不丢配置
- **TFT LCD 显示**：4.3 寸屏实时显示三路数据

### 网关端（树莓派）
- **Modbus 主站**：Python minimalmodbus 每秒轮询三路寄存器
- **实时推送**：Flask-SocketIO，WebSocket 主动推送（区别于前端轮询）
- **监控大屏**：三张实时卡片 + Chart.js 历史曲线 + 阈值报警（欠压 / 高温 / 过暗）
- **内网穿透**：cpolar，外网可访问

## 硬件组成

| 模块 | 型号 / 接口 | 引脚 | 说明 |
|---|---|---|---|
| 主控 | STM32F407ZGT6 | — | 正点原子探索者开发板 |
| 电压采集 | 电位器 RV1 | PA5（ADC1_IN5） | 0~3.3V 分压 |
| 温度采集 | 芯片内部温度传感器 | ADC1 | 内部通道 |
| 光强采集 | GL5506 光敏模块 | PF7（ADC3_IN5） | 分压电路 |
| RS485 | 板载 SP3485 | USART2（PA2/PA3）+ PG8 | 差分总线 |
| 显示屏 | 4.3 寸 TFT LCD | FSMC Bank4 | 8080 并口 |
| 存储 | MicroSD 卡 | SDIO | 日志落盘 |
| EEPROM | AT24C02 | I2C1 | 掉电保存 |
| 网关 | 树莓派 3B+ | USB 转 RS485 | Modbus 主站 |

## 目录结构

```
.
├── firmware/                    # STM32 固件（Keil MDK-ARM 工程）
│   ├── Core/                    # 用户代码（Src/Inc + BSP 驱动）
│   │   └── BSP/                 # LCD / LOG / SDIO / EEPROM
│   ├── Drivers/                 # STM32F4 HAL 库 + CMSIS
│   ├── Middlewares/             # FreeRTOS + FATFS
│   ├── MDK-ARM/                 # Keil 工程文件（.uvprojx）
│   └── Modbus_Slave.ioc         # CubeMX 配置
└── gateway/                     # 树莓派网关（Python）
    ├── app.py                   # Flask + SocketIO 服务
    └── templates/index.html     # 监控大屏前端
```

## 快速开始

### 1. STM32 固件
1. 用 Keil MDK-ARM 打开 `firmware/MDK-ARM/Modbus_Slave.uvprojx`
2. 编译（F7）→ 烧录（F8）到探索者板
3. 接线：电位器接 PA5，光敏模块接 PF7，RS485 的 A/B 接 USB 转 RS485 模块

### 2. 树莓派网关
```bash
pip install flask flask-socketio minimalmodbus pyserial --break-system-packages
python3 gateway/app.py
# 浏览器打开 http://<树莓派IP>:5000
```

### 3. 内网穿透（可选）
用 cpolar 将本地 5000 端口映射到公网，即可外网访问。

## 技术栈

`C` `STM32F407` `FreeRTOS` `CMSIS-RTOS2` `ADC + DMA` `Modbus-RTU` `RS485` `IWDG` `FATFS` `I2C` `FSMC` `Python` `Flask` `Flask-SocketIO` `minimalmodbus` `Chart.js` `cpolar`

## 项目亮点

- **分层解耦**：采集核心逻辑（ADC → 滤波 → 寄存器）从头到尾不变，只换传输方式（串口 / Modbus / RS485）和展示方式（串口 / LCD / 网页），即可完成形态升级
- **工业级稳定性**：看门狗 + 日志 + 滤波 + EEPROM，向企业级项目靠拢
- **端到端闭环**：从传感器到云端可视化完整打通
- **多任务并发**：FreeRTOS 任务划分 + 互斥锁保护共享资源

## 演示

（此处预留：实物照片 / 演示视频 / GIF）
