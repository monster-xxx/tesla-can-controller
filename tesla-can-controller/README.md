# Tesla Model Y 多功能CAN控制系统

## 🚗 项目概述

一个基于ESP32的Tesla Model Y多功能控制系统，实现CAN总线通信、车辆状态监控、高级功能控制和用户界面交互。

### 核心功能
- **车辆状态监控**：实时显示车速、电量、挡位、胎压等信息
- **高级控制功能**：灯光控制（领航灯、脉冲远光）、ESP关闭（漂移模式）、电池预热
- **性能测试**：百公里加速测试与历史记录
- **氛围灯控制**：12V WS2812 RGB氛围灯同步控制
- **智能界面**：M5Dial旋钮+触摸屏多级菜单系统

### 系统架构
```
┌─────────────────┐    加密BLE      ┌─────────────────┐
│  设备A: M5dial  │◄───────────────►│  设备B: ESP32-C6│
│   (主控制器)    │                 │   (CAN读取器)   │
│  - LVGL UI      │    加密BLE      │  - SN65HVD230   │
│  - 旋转编码器   │◄───────────────►│  - FreeRTOS     │
│  - 触摸屏       │                 │  - CAN协议解析  │
└─────────┬───────┘                 └─────────────────┘
          │加密BLE                           │CAN总线
          │                                 ▼
┌─────────▼───────┐                ┌─────────────────┐
│  设备C: ESP32   │                │   Tesla Model Y  │
│  (氛围灯控制)   │                │   CAN网络        │
│  - WS2812驱动   │                │   - 动力系统     │
│  - 12V电平转换  │                │   - 车身控制     │
│  - PWM调光      │                │   - 信息娱乐     │
└─────────────────┘                └─────────────────┘
```

## 📁 项目结构

```
tesla-can-controller/
├── README.md                    # 项目说明
├── docs/                        # 项目文档
│   ├── architecture.md          # 系统架构
│   ├── hardware_spec.md         # 硬件规格
│   ├── software_design.md       # 软件设计
│   ├── protocol_spec.md         # 通信协议
│   └── api_reference.md         # API参考
├── hardware/                    # 硬件设计
│   ├── schematics/              # 电路原理图
│   ├── pcb/                     # PCB设计文件
│   └── bom/                     # 物料清单
├── software/                    # 软件代码
│   ├── firmware/                # 设备固件
│   │   ├── device_a_m5dial/     # 设备A: M5Dial主控制器
│   │   ├── device_b_can_reader/ # 设备B: CAN读取器
│   │   └── device_c_ambilight/  # 设备C: 氛围灯控制器
│   └── tools/                   # 开发工具
│       ├── can_logger/          # CAN数据记录工具
│       ├── config_generator/    # 配置生成器
│       └── simulators/          # 车辆模拟器
└── resources/                   # 参考资料
    ├── tesla_dbc/               # Tesla DBC文件
    ├── datasheets/              # 芯片数据手册
    └── references/              # 参考文档
```

## 🔧 硬件要求

### 核心硬件
1. **设备A**: M5Dial ESP32-S3开发板 ×1
2. **设备B**: ESP32-C6开发板 ×1 + SN65HVD230 CAN收发器 ×2
3. **设备C**: ESP32开发板 ×1 + 12V WS2812 RGB灯带
4. **连接线缆**: OBD-II to DB9线缆、电平转换器、降压模块

### 可选硬件
- Tesla Key模拟器（ESP32-S3）
- 车辆连接线束和安全保险丝
- 3D打印外壳

## 🚀 快速开始

### 1. 一键开发环境搭建
```bash
# 克隆项目
git clone https://github.com/monster-xxx/tesla-can-controller.git
cd tesla-can-controller

# 运行开发环境安装脚本（需要sudo权限）
sudo ./tools/setup_dev_env.sh

# 设置开发环境
source ~/tesla-can-dev/setup_env.sh
```

### 2. 编译所有固件
```bash
# 一键编译所有设备固件
./tools/build_all.sh

# 或者分别编译
build-device-a   # 编译设备A (M5Dial)
build-device-b   # 编译设备B (CAN读取器)
build-device-c   # 编译设备C (氛围灯控制器)
```

### 3. 刷写固件
```bash
# 连接设备到对应串口后刷写
flash-device-a   # 刷写设备A (通常 /dev/ttyUSB0)
flash-device-b   # 刷写设备B (通常 /dev/ttyUSB1)
flash-device-c   # 刷写设备C (通常 /dev/ttyUSB2)
```

### 4. 监控输出
```bash
# 监控设备串口输出
monitor-device-a
monitor-device-b
monitor-device-c
```

### 3. 硬件连接
1. 将SN65HVD230模块连接到ESP32-C6和车辆CAN总线
2. 配置M5Dial的电源和显示连接
3. 连接氛围灯控制器到车辆12V电源和灯带

## 📋 功能列表

### 设备A (M5Dial) 功能
- [ ] 开机图标动画
- [ ] 默认显示画面
- [ ] 系统设置菜单
  - [ ] 匹配设备B
  - [ ] 亮度调节（同步车机主题）
  - [ ] 重启设备
  - [ ] 音量设置
  - [ ] WiFi固件升级
  - [ ] 关闭模块B
- [ ] 车辆信息显示
  - [ ] 车辆VIN
  - [ ] 电池信息
  - [ ] 动力电池状态
  - [ ] 单体电压
  - [ ] DCDC状态
  - [ ] 行驶里程
  - [ ] 维护模式
- [ ] ESP关闭（漂移模式）
- [ ] 仪表盘
  - [ ] 电池预热状态
  - [ ] 刹车温度
  - [ ] 挡位信息（PRND）
  - [ ] AP状态
  - [ ] 车速
  - [ ] 行驶里程
  - [ ] 胎压
  - [ ] 动能回收功率
  - [ ] 灯光状态
  - [ ] 剩余电量
- [ ] 百公里加速测试
- [ ] 领航灯控制
- [ ] 脉冲远光灯
- [ ] 氛围灯设置

### 设备B (CAN读取器) 功能
- [ ] CAN总线数据读取
- [ ] Tesla DBC协议解析
- [ ] BLE数据加密传输
- [ ] 车辆状态监控
- [ ] 控制命令执行

### 设备C (氛围灯控制器) 功能
- [ ] WS2812 RGB灯带控制
- [ ] 12V电平转换
- [ ] 多种灯光模式
- [ ] BLE远程控制

## 🔒 安全注意事项

### 必须实现的安全机制
1. **操作验证**：关键操作（关闭ESP）需要双重确认
2. **状态监控**：异常时自动恢复安全状态
3. **权限分级**：区分查看功能和控制功能
4. **数据加密**：BLE通信使用ECDH+AES-256-GCM加密
5. **固件签名**：OTA升级需要数字签名验证

### 法律与合规
1. **车辆保修**：改装可能影响Tesla保修
2. **道路法规**：灯光控制需符合当地交通法规
3. **安全标准**：符合车辆电子设备安全标准

## 📞 支持与贡献

### 问题反馈
- GitHub Issues: [提交问题](https://github.com/monster-xxx/tesla-can-controller/issues)
- 讨论区: [项目讨论](https://github.com/monster-xxx/tesla-can-controller/discussions)

### 贡献指南
1. Fork本仓库
2. 创建功能分支 (`git checkout -b feature/amazing-feature`)
3. 提交更改 (`git commit -m 'Add amazing feature'`)
4. 推送到分支 (`git push origin feature/amazing-feature`)
5. 打开Pull Request

## 📄 许可证

本项目采用 MIT 许可证 - 查看 [LICENSE](LICENSE) 文件了解详情。

## 🙏 致谢

- [Tesla Model 3 DBC](https://github.com/joshwardell/model3dbc) - Tesla CAN协议定义
- [ESP-IDF](https://github.com/espressif/esp-idf) - ESP32开发框架
- [LVGL](https://lvgl.io/) - 嵌入式图形库
- [M5Dial BSP](https://github.com/espressif/esp-bsp) - M5Dial开发板支持包

---

**开始开发日期**: 2026-03-21  
**最后更新**: 2026-03-21  
**状态**: 方案设计阶段