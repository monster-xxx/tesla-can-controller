# 通信协议规范

## 📡 协议概述

本系统使用两种主要通信协议：
1. **BLE (Bluetooth Low Energy)** - 设备间无线通信
2. **CAN (Controller Area Network)** - 车辆总线通信

## 🔵 BLE通信协议

### GATT服务定义

#### 主服务 (Primary Service)
```
服务UUID: 6E400001-B5A3-F393-E0A9-E50E24DCCA9E
服务名称: Tesla CAN Controller Service
```

#### 特性定义

##### 1. 命令接收特性 (RX)
```
特性UUID: 6E400002-B5A3-F393-E0A9-E50E24DCCA9E
权限: WRITE, WRITE_NO_RESPONSE
描述: 从设备A发送控制命令到设备B/C
```

##### 2. 数据发送特性 (TX)
```
特性UUID: 6E400003-B5A3-F393-E0A9-E50E24DCCA9E
权限: READ, NOTIFY
描述: 从设备B/C发送数据到设备A
```

##### 3. 通知特性
```
特性UUID: 6E400004-B5A3-F393-E0A9-E50E24DCCA9E
权限: NOTIFY
描述: 实时事件通知
```

### 数据包格式

#### 通用数据包头
```c
typedef struct __attribute__((packed)) {
    uint8_t  start_byte;     // 起始字节 (0xAA)
    uint8_t  version;        // 协议版本 (0x01)
    uint8_t  device_id;      // 设备ID (A=0x01, B=0x02, C=0x03)
    uint8_t  message_type;   // 消息类型
    uint16_t sequence;       // 序列号
    uint16_t data_length;    // 数据长度 (0-248)
    uint8_t  data[248];      // 数据载荷
    uint8_t  checksum;       // 校验和 (XOR of all bytes except checksum)
    uint8_t  end_byte;       // 结束字节 (0x55)
} ble_packet_t;
```

#### 消息类型定义
```c
typedef enum {
    // 系统消息
    MSG_SYSTEM_HEARTBEAT = 0x01,
    MSG_SYSTEM_STATUS    = 0x02,
    MSG_SYSTEM_ERROR     = 0x03,
    
    // 车辆数据
    MSG_VEHICLE_SPEED    = 0x10,
    MSG_VEHICLE_BATTERY  = 0x11,
    MSG_VEHICLE_GEAR     = 0x12,
    MSG_VEHICLE_TEMP     = 0x13,
    MSG_VEHICLE_TIRE     = 0x14,
    
    // 控制命令
    MSG_CTRL_ESP_OFF     = 0x20,
    MSG_CTRL_LIGHTS      = 0x21,
    MSG_CTRL_TEST_START  = 0x22,
    MSG_CTRL_TEST_STOP   = 0x23,
    
    // 配置消息
    MSG_CONFIG_SAVE      = 0x30,
    MSG_CONFIG_LOAD      = 0x31,
    MSG_CONFIG_RESET     = 0x32,
    
    // 诊断消息
    MSG_DIAG_REQUEST     = 0x40,
    MSG_DIAG_RESPONSE    = 0x41
} message_type_t;
```

### 具体消息格式

#### 车辆速度消息 (MSG_VEHICLE_SPEED)
```c
typedef struct __attribute__((packed)) {
    float speed_kmh;          // 车速 (km/h)
    float wheel_fl_kmh;       // 左前轮速 (km/h)
    float wheel_fr_kmh;       // 右前轮速 (km/h)
    float wheel_rl_kmh;       // 左后轮速 (km/h)
    float wheel_rr_kmh;       // 右后轮速 (km/h)
    uint32_t timestamp_ms;    // 时间戳 (毫秒)
} vehicle_speed_msg_t;
```

#### 电池状态消息 (MSG_VEHICLE_BATTERY)
```c
typedef struct __attribute__((packed)) {
    float voltage;            // 电池电压 (V)
    float current;            // 电池电流 (A)
    uint8_t soc;              // 剩余电量 (%)
    uint8_t soh;              // 电池健康度 (%)
    float temp_max;           // 最高温度 (°C)
    float temp_min;           // 最低温度 (°C)
    uint16_t charge_cycles;   // 充电循环次数
    uint32_t timestamp_ms;    // 时间戳 (毫秒)
} vehicle_battery_msg_t;
```

#### ESP关闭命令 (MSG_CTRL_ESP_OFF)
```c
typedef struct __attribute__((packed)) {
    uint8_t enable;           // 1=启用ESP关闭, 0=恢复ESP
    uint8_t confirmation;     // 确认码 (必须为0x5A)
    uint32_t timestamp_ms;    // 时间戳 (毫秒)
} esp_off_cmd_t;
```

#### 灯光控制命令 (MSG_CTRL_LIGHTS)
```c
typedef struct __attribute__((packed)) {
    uint8_t light_type;       // 灯光类型 (0=领航灯, 1=脉冲远光, 2=氛围灯)
    uint8_t mode;             // 模式 (0=关, 1=开, 2=自动, 3=自定义)
    uint8_t brightness;       // 亮度 (0-100%)
    uint8_t color_r;          // 红色分量 (0-255)
    uint8_t color_g;          // 绿色分量 (0-255)
    uint8_t color_b;          // 蓝色分量 (0-255)
    uint16_t duration_ms;     // 持续时间 (毫秒, 0=持续)
} lights_cmd_t;
```

### 加密协议

#### 密钥交换流程
```
1. 设备A (中央设备) 发起连接
2. 设备B/C (外围设备) 发送公钥
3. 设备A 计算共享密钥
4. 双方使用HKDF派生会话密钥
5. 开始加密通信
```

#### 加密数据包格式
```c
typedef struct __attribute__((packed)) {
    uint8_t  iv[12];          // 初始化向量 (96-bit)
    uint8_t  ciphertext[248]; // 密文
    uint8_t  auth_tag[16];    // 认证标签 (128-bit)
} encrypted_packet_t;
```

## 🚗 CAN通信协议

### Tesla Model Y CAN总线配置

#### 总线参数
```
高速CAN (HS-CAN):
- 波特率: 500 kbps
- 终端电阻: 120Ω
- 主要网络: 动力系统、底盘控制

低速CAN (LS-CAN):
- 波特率: 125 kbps
- 终端电阻: 120Ω
- 主要网络: 车身控制、舒适系统
```

#### 关键CAN消息ID

| CAN ID | 名称 | 周期 | 描述 |
|--------|------|------|------|
| 0x1A6 | 车速信息 | 20ms | 车速、轮速 |
| 0x352 | BMS信息 | 100ms | 电池状态 |
| 0x108 | 挡位信息 | 50ms | 挡位位置 |
| 0x2F4 | 胎压信息 | 500ms | 胎压监测 |
| 0x3D4 | 温度信息 | 200ms | 电池温度 |
| 0x456 | 充电状态 | 100ms | 充电信息 |
| 0x5A2 | 灯光状态 | 50ms | 外部灯光 |
| 0x6B8 | 车门状态 | 100ms | 车门/车窗 |

### DBC信号定义

#### 车速信号 (ID: 0x1A6)
```
BO_ 422 Speed_Info: 8 VCU
 SG_ VehicleSpeed : 7|16@1+ (0.01,0) [0|655.35] "km/h"  EPS,IC
 SG_ WheelSpeed_FL : 23|16@1+ (0.01,0) [0|655.35] "km/h" EPS,IC
 SG_ WheelSpeed_FR : 39|16@1+ (0.01,0) [0|655.35] "km/h" EPS,IC
 SG_ WheelSpeed_RL : 55|16@1+ (0.01,0) [0|655.35] "km/h" EPS,IC
 SG_ WheelSpeed_RR : 71|16@1+ (0.01,0) [0|655.35] "km/h" EPS,IC
```

#### 电池状态信号 (ID: 0x352)
```
BO_ 850 BMS_Info: 8 BMS
 SG_ BatteryVoltage : 7|16@1+ (0.1,0) [0|6553.5] "V" VCU,IC
 SG_ BatteryCurrent : 23|16@1- (0.1,-3276.8) [-3276.8|3276.7] "A" VCU,IC
 SG_ SOC : 39|8@1+ (0.5,0) [0|127.5] "%" VCU,IC
 SG_ SOH : 47|8@1+ (0.5,0) [0|127.5] "%" VCU,IC
 SG_ TempMax : 55|8@1- (0.5,-64) [-64|63.5] "°C" VCU,IC
 SG_ TempMin : 63|8@1- (0.5,-64) [-64|63.5] "°C" VCU,IC
```

#### 挡位信号 (ID: 0x108)
```
BO_ 264 Gear_Info: 8 VCU
 SG_ GearPosition : 7|3@1+ (1,0) [0|7] "" IC,EPS
   VAL_ 0 Park 1 Reverse 2 Neutral 3 Drive 4 Sport 5 Low 6 Manual 7 Invalid
 SG_ GearValid : 10|1@1+ (1,0) [0|1] "" IC
 SG_ GearLeverPosition : 11|4@1+ (1,0) [0|15] "" IC
```

### 控制命令CAN消息

#### ESP关闭命令 (ID: 0x7E0)
```
BO_ 2016 ESP_Control: 8 Custom
 SG_ ESP_Off_Request : 7|1@1+ (1,0) [0|1] "" VCU
   VAL_ 0 ESP_Normal 1 ESP_Off
 SG_ ConfirmationCode : 8|8@1+ (1,0) [0|255] "" VCU
   VAL_ 90 0x5A_Confirmed
 SG_ RequestID : 16|16@1+ (1,0) [0|65535] "" VCU
```

#### 灯光控制命令 (ID: 0x7E1)
```
BO_ 2017 Lights_Control: 8 Custom
 SG_ LightType : 7|3@1+ (1,0) [0|7] "" BCM
   VAL_ 0 Navigation 1 PulseHighBeam 2 Ambient
 SG_ LightCommand : 10|2@1+ (1,0) [0|3] "" BCM
   VAL_ 0 Off 1 On 2 Auto 3 Custom
 SG_ Brightness : 12|7@1+ (1,0) [0|100] "%" BCM
 SG_ ColorRed : 19|8@1+ (1,0) [0|255] "" BCM
 SG_ ColorGreen : 27|8@1+ (1,0) [0|255] "" BCM
 SG_ ColorBlue : 35|8@1+ (1,0) [0|255] "" BCM
```

## 🔄 通信流程

### 正常操作流程
```
1. 设备上电初始化
2. BLE设备发现和配对
3. CAN总线监听启动
4. 周期性数据采集
5. 实时数据显示
6. 用户交互处理
7. 控制命令执行
```

### 数据采集流程
```
车辆传感器 → CAN总线 → 设备B接收 → DBC解析 → 
数据聚合 → BLE加密 → 设备A接收 → UI更新 → 用户查看
```

### 控制命令流程
```
用户操作 → 设备A UI → 命令生成 → BLE加密 → 
设备B接收 → 命令验证 → CAN命令转换 → 车辆执行 → 
响应返回 → BLE返回 → UI状态更新
```

## ⚡ 性能要求

### 实时性指标
| 指标 | 要求 | 说明 |
|------|------|------|
| CAN数据延迟 | < 50ms | 传感器到UI显示 |
| BLE传输延迟 | < 20ms | 设备间数据传输 |
| UI响应时间 | < 100ms | 用户操作反馈 |
| 控制命令延迟 | < 100ms | 命令到执行 |

### 可靠性指标
| 指标 | 目标值 | 测试方法 |
|------|------|------|
| 数据完整性 | > 99.99% | CRC校验统计 |
| 连接稳定性 | > 99.9% | BLE断开率 |
| 命令成功率 | > 99.5% | 控制命令统计 |
| 系统可用性 | > 99.9% | 运行时间统计 |

### 安全要求
| 要求 | 实现方式 | 验证方法 |
|------|----------|----------|
| 数据加密 | AES-256-GCM | 加密测试 |
| 身份认证 | ECDH密钥交换 | 认证测试 |
| 防重放攻击 | 序列号+时间戳 | 安全测试 |
| 权限控制 | 命令分级 | 功能测试 |

## 🛠️ 调试和诊断

### 调试接口
```
串口调试 (UART0):
- 波特率: 115200
- 数据位: 8
- 停止位: 1
- 校验位: 无

调试命令:
- help: 显示帮助信息
- status: 显示系统状态
- can_stats: CAN统计信息
- ble_stats: BLE统计信息
- reset: 系统重启
- config: 配置管理
```

### 诊断协议
```
诊断请求格式:
[命令][参数长度][参数数据][校验]

诊断命令:
0x01: 读取CAN消息统计
0x02: 读取BLE连接状态
0x03: 读取设备信息
0x04: 读取错误日志
0x05: 清除错误日志
0x06: 系统自检
0x07: 固件版本查询
```

### 错误代码
```c
typedef enum {
    ERROR_NONE = 0,
    ERROR_CAN_INIT_FAILED,
    ERROR_CAN_RX_TIMEOUT,
    ERROR_CAN_TX_FAILED,
    ERROR_BLE_INIT_FAILED,
    ERROR_BLE_CONNECT_FAILED,
    ERROR_BLE_DISCONNECTED,
    ERROR_DBC_PARSE_FAILED,
    ERROR_MEMORY_ALLOC_FAILED,
    ERROR_CRYPTO_FAILED,
    ERROR_INVALID_COMMAND,
    ERROR_PERMISSION_DENIED,
    ERROR_VEHICLE_STATE_INVALID,
    ERROR_MAX
} error_code_t;
```

## 📊 数据记录

### 数据记录格式
```c
typedef struct __attribute__((packed)) {
    uint32_t timestamp;      // 时间戳 (Unix时间)
    uint8_t  device_id;      // 设备ID
    uint8_t  message_type;   // 消息类型
    uint16_t data_length;    // 数据长度
    uint8_t  data[240];      // 数据内容
    uint8_t  checksum;       // 校验和
} data_log_entry_t;
```

### 记录策略
```
实时数据: 存储最近1小时数据 (循环缓冲区)
事件数据: 永久存储 (SD卡/Flash)
统计数据: 每日汇总
错误日志: 永久存储
```

---

**协议版本**: 1.0  
**最后更新**: 2026-03-21  
**兼容性**: 向后兼容  
**状态**: 设计完成，待实现