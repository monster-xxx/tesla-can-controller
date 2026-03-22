# 软件设计文档

## 🏗️ 软件架构概述

Tesla CAN控制器系统由三个独立的嵌入式设备组成，每个设备运行独立的FreeRTOS固件，通过加密BLE通信协同工作。

### 整体架构
```
┌─────────────────────────────────────────────────────────┐
│                   应用层 (Application Layer)              │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐    │
│  │  设备A: UI   │  │  设备B: CAN  │  │  设备C: 灯光  │    │
│  │  控制器      │  │  读取器      │  │  控制器      │    │
│  └──────┬──────┘  └──────┬──────┘  └──────┬──────┘    │
│         │                 │                 │          │
├─────────┼─────────────────┼─────────────────┼──────────┤
│         │                 │                 │          │
│  ┌──────▼──────┐  ┌──────▼──────┐  ┌──────▼──────┐    │
│  │  中间件层     │  │  中间件层     │  │  中间件层     │    │
│  │  (Middleware) │  │  (Middleware) │  │  (Middleware) │    │
│  └──────┬──────┘  └──────┬──────┘  └──────┬──────┘    │
│         │                 │                 │          │
├─────────┼─────────────────┼─────────────────┼──────────┤
│         │                 │                 │          │
│  ┌──────▼──────┐  ┌──────▼──────┐  ┌──────▼──────┐    │
│  │  硬件抽象层   │  │  硬件抽象层   │  │  硬件抽象层   │    │
│  │   (HAL)     │  │   (HAL)     │  │   (HAL)     │    │
│  └──────┬──────┘  └──────┬──────┘  └──────┬──────┘    │
│         │                 │                 │          │
└─────────┼─────────────────┼─────────────────┼──────────┘
          │                 │                 │
    ┌─────▼─────┐     ┌─────▼─────┐     ┌─────▼─────┐
    │   M5Dial  │     │ ESP32-C6  │     │  ESP32   │
    │ 硬件平台   │     │ 硬件平台   │     │ 硬件平台   │
    └───────────┘     └───────────┘     └───────────┘
```

## 📱 设备A: M5Dial主控制器

### 任务架构
```
主任务 (Main Task)
├── 系统初始化
├── 任务创建
└── 系统监控

UI任务 (UI Task) - 优先级: 5
├── LVGL初始化
├── 屏幕管理
├── 触摸/旋钮处理
├── 动画渲染
└── 主题管理

BLE任务 (BLE Task) - 优先级: 4
├── BLE中央设备初始化
├── 设备扫描和连接
├── 数据收发处理
├── 加密解密
└── 连接管理

车辆状态任务 (Vehicle Task) - 优先级: 3
├── 车辆数据解析
├── 状态机更新
├── 事件检测
└── 历史数据记录

控制任务 (Control Task) - 优先级: 4
├── 用户命令处理
├── 安全验证
├── 命令发送
└── 响应处理
```

### 关键数据结构

#### 车辆状态结构
```c
typedef struct {
    // 基本信息
    float speed_kmh;
    uint8_t soc_percent;
    uint8_t gear_position;
    float battery_temp_c;
    
    // 电池信息
    float battery_voltage;
    float battery_current;
    uint8_t soh_percent;
    uint16_t charge_cycles;
    
    // 轮胎信息
    float tire_pressure_fl;
    float tire_pressure_fr;
    float tire_pressure_rl;
    float tire_pressure_rr;
    
    // 系统状态
    bool esp_active;
    bool navigation_lights;
    bool pulse_high_beam;
    bool ambient_lights;
    
    // 时间戳
    uint32_t last_update_ms;
    uint32_t vehicle_uptime_s;
} vehicle_state_t;
```

#### UI状态结构
```c
typedef struct {
    // 屏幕状态
    screen_id_t current_screen;
    screen_id_t previous_screen;
    
    // 菜单状态
    menu_item_t selected_item;
    uint8_t menu_scroll_offset;
    
    // 显示设置
    uint8_t brightness;
    uint32_t theme_primary;
    uint32_t theme_secondary;
    
    // 动画状态
    bool boot_animation_complete;
    bool screen_transition_active;
    
    // 输入状态
    int32_t encoder_position;
    bool encoder_pressed;
    touch_point_t last_touch;
} ui_state_t;
```

### 主要算法

#### 屏幕切换算法
```c
void ui_manager_switch_screen(screen_id_t new_screen) {
    // 1. 验证屏幕ID有效性
    if (new_screen >= SCREEN_MAX) return;
    
    // 2. 保存当前屏幕
    ui_state.previous_screen = ui_state.current_screen;
    
    // 3. 执行离开动画（如果支持）
    if (ui_state.screen_transition_active) {
        screen_transition_leave(ui_state.current_screen);
    }
    
    // 4. 清理当前屏幕资源
    screen_cleanup(ui_state.current_screen);
    
    // 5. 加载新屏幕资源
    screen_load(new_screen);
    
    // 6. 执行进入动画
    if (ui_state.screen_transition_active) {
        screen_transition_enter(new_screen);
    }
    
    // 7. 更新状态
    ui_state.current_screen = new_screen;
    
    // 8. 触发屏幕更新事件
    event_post(EVENT_SCREEN_CHANGED, &new_screen, sizeof(new_screen));
}
```

#### BLE设备发现算法
```c
void ble_central_scan_devices(void) {
    // 1. 开始扫描
    esp_ble_gap_start_scanning(SCAN_DURATION);
    
    // 2. 过滤设备
    ble_device_t found_devices[MAX_DEVICES];
    uint8_t device_count = 0;
    
    while (scanning_active) {
        // 3. 检查广告数据
        if (check_advertisement_data(adv_data)) {
            // 4. 验证设备类型
            if (is_tesla_device(adv_data)) {
                // 5. 添加到发现列表
                found_devices[device_count++] = create_device_entry(adv_data);
                
                // 6. 检查是否找到所有设备
                if (device_count >= REQUIRED_DEVICES) {
                    break;
                }
            }
        }
        
        // 7. 超时检查
        if (scan_timeout_reached()) {
            break;
        }
    }
    
    // 8. 停止扫描
    esp_ble_gap_stop_scanning();
    
    // 9. 连接设备
    for (int i = 0; i < device_count; i++) {
        connect_to_device(&found_devices[i]);
    }
}
```

## 🔌 设备B: CAN读取器

### 任务架构
```
主任务 (Main Task)
├── 系统初始化
├── CAN控制器初始化
├── BLE外设初始化
└── 任务创建

CAN接收任务 (CAN RX Task) - 优先级: 6
├── CAN消息接收
├── 消息过滤
├── DBC解析
├── 数据聚合
└── 队列管理

CAN发送任务 (CAN TX Task) - 优先级: 5
├── 命令接收
├── CAN消息构造
├── 发送调度
└── 错误处理

BLE任务 (BLE Task) - 优先级: 4
├── GATT服务初始化
├── 连接处理
├── 数据收发
└── 加密处理

数据处理任务 (Data Task) - 优先级: 3
├── 车辆状态计算
├── 事件检测
├── 数据压缩
└── 存储管理
```

### 关键数据结构

#### CAN消息缓存
```c
typedef struct {
    twai_message_t messages[CAN_BUFFER_SIZE];
    uint16_t head;
    uint16_t tail;
    uint16_t count;
    SemaphoreHandle_t mutex;
} can_buffer_t;

typedef struct {
    // CAN原始数据
    uint32_t can_id;
    uint8_t data[8];
    uint8_t data_length;
    uint32_t timestamp_ms;
    
    // 解析后数据
    float vehicle_speed;
    float battery_voltage;
    uint8_t soc_percent;
    uint8_t gear_position;
    
    // 状态标志
    bool parsed;
    bool valid;
    bool transmitted;
} can_message_t;
```

#### DBC解析上下文
```c
typedef struct {
    // DBC数据库
    dbc_message_t *messages;
    uint16_t message_count;
    
    // 信号缓存
    dbc_signal_t signals[MAX_SIGNALS];
    uint16_t signal_count;
    
    // 解析状态
    uint32_t parse_errors;
    uint32_t parse_success;
    float parse_time_avg_ms;
    
    // 缓存
    uint8_t raw_buffer[8];
    float signal_buffer[MAX_SIGNALS];
} dbc_context_t;
```

### 主要算法

#### CAN消息处理流水线
```c
void can_message_pipeline(twai_message_t *raw_msg) {
    // 阶段1: 消息验证
    if (!validate_can_message(raw_msg)) {
        log_error("Invalid CAN message");
        return;
    }
    
    // 阶段2: 消息过滤
    if (!filter_can_message(raw_msg->identifier)) {
        // 消息被过滤，不处理
        return;
    }
    
    // 阶段3: DBC解析
    parsed_message_t parsed = dbc_parse_message(raw_msg);
    if (!parsed.valid) {
        log_warning("Failed to parse CAN message");
        return;
    }
    
    // 阶段4: 数据转换
    converted_data_t converted = convert_can_data(&parsed);
    
    // 阶段5: 数据聚合
    aggregate_vehicle_data(&converted);
    
    // 阶段6: 事件检测
    check_for_events(&converted);
    
    // 阶段7: BLE封装
    ble_packet_t packet = create_ble_packet(&converted);
    
    // 阶段8: 发送到队列
    if (xQueueSend(ble_tx_queue, &packet, QUEUE_TIMEOUT) != pdTRUE) {
        log_error("Failed to queue BLE packet");
    }
    
    // 阶段9: 数据记录
    if (should_log_data(&converted)) {
        log_data_entry(&converted);
    }
}
```

#### 车辆状态机
```c
void update_vehicle_state_machine(vehicle_state_t *state, can_data_t *data) {
    static vehicle_state_t previous_state;
    
    // 1. 更新基本状态
    state->speed_kmh = data->vehicle_speed;
    state->soc_percent = data->battery_soc;
    state->gear_position = data->gear_position;
    
    // 2. 检查状态变化
    if (state->speed_kmh != previous_state.speed_kmh) {
        event_post(EVENT_SPEED_CHANGED, &state->speed_kmh, sizeof(float));
    }
    
    if (state->soc_percent != previous_state.soc_percent) {
        event_post(EVENT_SOC_CHANGED, &state->soc_percent, sizeof(uint8_t));
    }
    
    // 3. 检查警告条件
    check_warnings(state);
    
    // 4. 更新历史数据
    update_history(state);
    
    // 5. 保存当前状态
    memcpy(&previous_state, state, sizeof(vehicle_state_t));
}
```

## 💡 设备C: 氛围灯控制器

### 任务架构
```
主任务 (Main Task)
├── 系统初始化
├── LED控制器初始化
├── BLE外设初始化
└── 任务创建

灯光控制任务 (LED Task) - 优先级: 5
├── WS2812驱动
├── 颜色计算
├── 效果渲染
├── 亮度调节
└── 温度管理

BLE任务 (BLE Task) - 优先级: 4
├── GATT服务初始化
├── 命令接收
├── 状态上报
└── 连接管理

场景管理任务 (Scene Task) - 优先级: 3
├── 场景切换
├── 效果过渡
├── 音乐同步
└── 自动模式

电源管理任务 (Power Task) - 优先级: 2
├── 电压监测
├── 电流限制
├── 温度监控
└── 保护控制
```

### 关键数据结构

#### 灯光状态结构
```c
typedef struct {
    // 当前颜色
    uint8_t red;
    uint8_t green;
    uint8_t blue;
    uint8_t white;
    uint8_t brightness;
    
    // 目标颜色（用于渐变）
    uint8_t target_red;
    uint8_t target_green;
    uint8_t target_blue;
    uint8_t target_brightness;
    
    // 效果参数
    light_effect_t effect;
    uint16_t effect_speed;
    uint8_t effect_intensity;
    
    // 场景状态
    light_scene_t scene;
    uint8_t scene_progress;
    bool scene_loop;
    
    // 时间控制
    uint32_t transition_start_ms;
    uint16_t transition_duration_ms;
} light_state_t;
```

#### LED缓冲区
```c
typedef struct {
    // LED数据
    led_color_t leds[MAX_LEDS];
    uint16_t led_count;
    
    // 缓冲区状态
    bool buffer_dirty;
    uint32_t last_update_ms;
    uint16_t update_interval_ms;
    
    // 渲染状态
    uint16_t current_frame;
    uint16_t total_frames;
    bool rendering_active;
    
    // 同步信息
    uint32_t sync_timestamp;
    uint8_t sync_group;
} led_buffer_t;
```

### 主要算法

#### 灯光效果渲染算法
```c
void render_light_effect(led_buffer_t *buffer, light_state_t *state) {
    switch (state->effect) {
        case EFFECT_SOLID:
            render_solid_color(buffer, state);
            break;
            
        case EFFECT_GRADIENT:
            render_gradient(buffer, state);
            break;
            
        case EFFECT_BREATHING:
            render_breathing(buffer, state);
            break;
            
        case EFFECT_RAINBOW:
            render_rainbow(buffer, state);
            break;
            
        case EFFECT_MUSIC:
            render_music_sync(buffer, state);
            break;
            
        case EFFECT_CUSTOM:
            render_custom_pattern(buffer, state);
            break;
            
        default:
            // 默认效果
            render_solid_color(buffer, state);
            break;
    }
    
    // 应用亮度
    apply_brightness(buffer, state->brightness);
    
    // 应用颜色校正
    apply_color_correction(buffer);
    
    // 标记缓冲区为脏
    buffer->buffer_dirty = true;
    buffer->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
}
```

#### 颜色渐变算法
```c
void calculate_color_transition(light_state_t *state) {
    uint32_t current_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
    uint32_t elapsed = current_time - state->transition_start_ms;
    
    if (elapsed >= state->transition_duration_ms) {
        // 过渡完成
        state->red = state->target_red;
        state->green = state->target_green;
        state->blue = state->target_blue;
        state->brightness = state->target_brightness;
        return;
    }
    
    // 计算过渡比例 (0.0 - 1.0)
    float ratio = (float)elapsed / state->transition_duration_ms;
    
    // 应用缓动函数
    ratio = ease_in_out_cubic(ratio);
    
    // 计算过渡颜色
    state->red = interpolate_uint8(state->red, state->target_red, ratio);
    state->green = interpolate_uint8(state->green, state->target_green, ratio);
    state->blue = interpolate_uint8(state->blue, state->target_blue, ratio);
    state->brightness = interpolate_uint8(state->brightness, state->target_brightness, ratio);
}

// 缓动函数：三次缓入缓出
float ease_in_out_cubic(float x) {
    return x < 0.5 ? 4 * x * x * x : 1 - pow(-2 * x + 2, 3) / 2;
}

// 8位整数插值
uint8_t interpolate_uint8(uint8_t start, uint8_t end, float ratio) {
    return start + (uint8_t)((end - start) * ratio);
}
```

## 🔒 安全机制

### 加密通信流程
```c
bool secure_communication_handshake(device_t *device) {
    // 阶段1: 密钥交换
    if (!perform_key_exchange(device)) {
        log_error("Key exchange failed");
        return false;
    }
    
    // 阶段2: 会话密钥派生
    if (!derive_session_key(device)) {
        log_error("Session key derivation failed");
        return false;
    }
    
    // 阶段3: 相互认证
    if (!mutual_authentication(device)) {
        log_error("Mutual authentication failed");
        return false;
    }
    
    // 阶段4: 安全通道建立
    if (!establish_secure_channel(device)) {
        log_error("Secure channel establishment failed");
        return false;
    }
    
    return true;
}
```

### 命令验证算法
```c
bool validate_control_command(control_command_t *cmd) {
    // 1. 检查命令结构有效性
    if (cmd == NULL || cmd->header != CMD_HEADER_MAGIC) {
        return false;
    }
    
    // 2. 验证CRC校验
    uint32_t calculated_crc = calculate_crc32(cmd, sizeof(*cmd) - sizeof(cmd->crc));
    if (calculated_crc != cmd->crc) {
        log_error("CRC mismatch");
        return false;
    }
    
    // 3. 检查时间戳（防重放攻击）
    uint32_t current_time = get_current_timestamp();
    if (cmd->timestamp > current_time || 
        (current_time - cmd->timestamp) > MAX_COMMAND_AGE_MS) {
        log_error("Invalid timestamp");
        return false;
    }
    
    // 4. 检查序列号
    if (!check_sequence_number(cmd->sequence)) {
        log_error("Invalid sequence number");
        return false;
    }
    
    // 5. 权限检查
    if (!has_permission_for_command(cmd->type, cmd->source)) {
        log_error("Permission denied");
        return false;
    }
    
    // 6. 参数范围检查
    if (!validate_command_parameters(cmd)) {
        log_error("Invalid parameters");
        return false;
    }
    
    // 7. 车辆状态检查（安全相关命令）
    if (is_safety_critical_command(cmd->type)) {
        if (!validate_vehicle_state_for_command(cmd)) {
            log_error("Vehicle state not suitable for command");
            return false;
        }
    }
    
    return true;
}
```

## 📊 数据管理

### 数据存储架构
```
SPIFFS文件系统 (设备A/B/C)
├── /config/
│   ├── system.cfg      # 系统配置
│   ├── ble.cfg         # BLE配置
│   ├── can.cfg         # CAN配置
│   └── ui.cfg          # UI配置
├── /logs/
│   ├── error.log       # 错误日志
│   ├── access.log      # 访问日志
│   └── can.log         # CAN数据日志
├── /data/
│   ├── vehicle.dat     # 车辆数据
│   ├── performance.dat # 性能测试数据
│   └── history.dat     # 历史数据
└── /ota/
    ├── firmware.bin    # OTA固件
    └── metadata.json   # 固件元数据
```

### 配置管理
```c
typedef struct {
    // 系统配置
    uint8_t device_id;
    char device_name[32];
    uint32_t serial_number;
    
    // BLE配置
    char ble_name[32];
    uint8_t ble_tx_power;
    uint16_t ble_interval_ms;
    uint16_t ble_slave_latency;
    uint16_t ble_timeout_ms;
    
    // CAN配置
    uint32_t can_bitrate;
    uint8_t can_mode;
    uint32_t can_filter_ids[8];
    uint8_t can_filter_count;
    
    // UI配置
    uint8_t ui_brightness;
    uint32_t ui_theme_primary;
    uint32_t ui_theme_secondary;
    uint8_t ui_language;
    bool ui_animations;
    
    // 安全配置
    uint8_t security_level;
    bool require_confirmation;
    uint8_t max_failed_attempts;
    uint32_t lockout_duration_ms;
    
    // 版本信息
    uint32_t firmware_version;
    uint32_t config_version;
    uint32_t last_updated;
} system_config_t;
```

## 🔄 任务调度

### FreeRTOS任务配置
```c
// 设备A任务配置
static const task_config_t device_a_tasks[] = {
    {
        .name = "ui_task",
        .function = ui_task,
        .stack_size = 8192,
        .priority = 5,
        .core_id = 0,
        .parameters = NULL
    },
    {
        .name = "ble_task",
        .function = ble_task,
        .stack_size = 4096,
        .priority = 4,
        .core_id = 1,
        .parameters = NULL
    },
    {
        .name = "vehicle_task",
        .function = vehicle_task,
        .stack_size = 4096,
        .priority = 3,
        .core_id = 0,
        .parameters = NULL
    }
};

// 任务创建函数
void create_all_tasks(void) {
    for (int i = 0; i < ARRAY_SIZE(device_a_tasks); i++) {
        xTaskCreatePinnedToCore(
            device_a_tasks[i].function,
            device_a_tasks[i].name,
            device_a_tasks[i].stack_size,
            device_a_tasks[i].parameters,
            device_a_tasks[i].priority,
            NULL,
            device_a_tasks[i].core_id
        );
    }
}
```

### 中断处理
```c
// CAN接收中断
static void IRAM_ATTR can_rx_isr(void *arg) {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    
    // 读取CAN消息
    twai_message_t message;
    if (twai_receive(&message, 0) == ESP_OK) {
        // 发送到队列
        xQueueSendFromISR(can_rx_queue, &message, &xHigherPriorityTaskWoken);
    }
    
    // 如果需要上下文切换
    if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR();
    }
}

// 旋钮编码器中断
static void IRAM_ATTR encoder_isr(void *arg) {
    static int32_t last_position = 0;
    int32_t current_position = read_encoder_position();
    int32_t delta = current_position - last_position;
    
    if (delta != 0) {
        // 发送编码器事件
        encoder_event_t event = {
            .delta = delta,
            .timestamp = xTaskGetTickCountFromISR()
        };
        xQueueSendFromISR(encoder_queue, &event, NULL);
        last_position = current_position;
    }
}
```

## 🧪 测试框架

### 单元测试结构
```c
// 测试用例定义
TEST_CASE("CAN消息解析", "[can]") {
    // 准备测试数据
    twai_message_t test_msg = {
        .identifier = 0x1A6,
        .data_length_code = 8,
        .data = {0x00, 0x64, 0x00, 0x63, 0x00, 0x62, 0x00, 0x61}
    };
    
    // 执行测试
    parsed_message_t result = dbc_parse_message(&test_msg);
    
    // 验证结果
    TEST_ASSERT_TRUE(result.valid);
    TEST_ASSERT_EQUAL_FLOAT(1.00f, result.vehicle_speed);
    TEST_ASSERT_EQUAL_FLOAT(0.99f, result.wheel_speed_fl);
    TEST_ASSERT_EQUAL_FLOAT(0.98f, result.wheel_speed_fr);
    TEST_ASSERT_EQUAL_FLOAT(0.97f, result.wheel_speed_rl);
    TEST_ASSERT_EQUAL_FLOAT(0.96f, result.wheel_speed_rr);
}

// 集成测试
TEST_CASE("端到端数据流", "[integration]") {
    // 模拟CAN数据
    simulate_can_data();
    
    // 验证BLE传输
    verify_ble_transmission();
    
    // 验证UI更新
    verify_ui_update();
    
    // 验证数据存储
    verify_data_storage();
}
```

### 性能测试
```c
void run_performance_tests(void) {
    printf("性能测试开始...\n");
    
    // 延迟测试
    test_latency();
    
    // 吞吐量测试
    test_throughput();
    
    // 内存使用测试
    test_memory_usage();
    
    // CPU使用率测试
    test_cpu_usage();
    
    // 功耗测试
    test_power_consumption();
    
    printf("性能测试完成\n");
}

void test_latency(void) {
    uint32_t start_time, end_time;
    uint32_t total_latency = 0;
    const int iterations = 1000;
    
    for (int i = 0; i < iterations; i++) {
        start_time = esp_timer_get_time();
        
        // 执行测试操作
        process_test_message();
        
        end_time = esp_timer_get_time();
        total_latency += (end_time - start_time);
    }
    
    float avg_latency_us = total_latency / (float)iterations;
    printf("平均延迟: %.2f μs\n", avg_latency_us);
}
```

## 🐛 调试和诊断

### 调试接口
```c
// 调试命令处理器
void debug_command_handler(char *command) {
    if (strcmp(command, "status") == 0) {
        print_system_status();
    } else if (strcmp(command, "tasks") == 0) {
        print_task_status();
    } else if (strcmp(command, "memory") == 0) {
        print_memory_usage();
    } else if (strcmp(command, "can") == 0) {
        print_can_statistics();
    } else if (strcmp(command, "ble") == 0) {
        print_ble_statistics();
    } else if (strcmp(command, "config") == 0) {
        print_configuration();
    } else if (strcmp(command, "reset") == 0) {
        perform_system_reset();
    } else {
        printf("未知命令: %s\n", command);
        print_help();
    }
}

// 系统状态监控
void monitor_system_health(void) {
    static uint32_t last_check = 0;
    uint32_t current_time = xTaskGetTickCount();
    
    if (current_time - last_check > HEALTH_CHECK_INTERVAL) {
        // 检查任务状态
        check_task_health();
        
        // 检查内存状态
        check_memory_health();
        
        // 检查外设状态
        check_peripheral_health();
        
        // 记录健康状态
        log_health_status();
        
        last_check = current_time;
    }
}
```

### 错误处理
```c
// 错误处理函数
void handle_error(error_code_t error, const char *file, int line) {
    // 记录错误
    error_log_entry_t entry = {
        .timestamp = get_current_timestamp(),
        .error_code = error,
        .file = file,
        .line = line,
        .task_name = pcTaskGetName(NULL)
    };
    
    log_error_entry(&entry);
    
    // 根据错误级别处理
    switch (get_error_level(error)) {
        case ERROR_LEVEL_INFO:
            // 仅记录，不采取行动
            break;
            
        case ERROR_LEVEL_WARNING:
            // 记录并通知用户
            notify_user_warning(error);
            break;
            
        case ERROR_LEVEL_ERROR:
            // 尝试恢复
            attempt_recovery(error);
            break;
            
        case ERROR_LEVEL_CRITICAL:
            // 安全关闭系统
            safe_shutdown();
            break;
            
        default:
            // 未知错误级别
            safe_shutdown();
            break;
    }
}

// 错误恢复策略
bool attempt_recovery(error_code_t error) {
    switch (error) {
        case ERROR_CAN_INIT_FAILED:
            // 重新初始化CAN控制器
            return reinitialize_can();
            
        case ERROR_BLE_DISCONNECTED:
            // 重新连接BLE
            return reconnect_ble();
            
        case ERROR_MEMORY_ALLOC_FAILED:
            // 清理内存并重试
            cleanup_memory();
            return true;
            
        default:
            // 通用恢复：重启相关模块
            return restart_module(get_error_module(error));
    }
}
```

## 📈 性能优化

### 内存优化技巧
```c
// 使用静态分配避免碎片
static uint8_t can_buffer[CAN_BUFFER_SIZE][8];
static light_color_t led_buffer[LED_COUNT];

// 使用内存池
typedef struct {
    uint8_t *buffer;
    size_t size;
    size_t used;
} memory_pool_t;

// 优化数据结构大小
typedef struct __attribute__((packed)) {
    uint32_t timestamp : 24;  // 24位时间戳（最大16.7秒）
    uint8_t device_id : 3;    // 3位设备ID（0-7）
    uint8_t message_type : 5; // 5位消息类型（0-31）
    uint16_t data;            // 16位数据
} compact_message_t;
```

### 功耗优化
```c
void optimize_power_consumption(void) {
    // 1. 动态频率调整
    set_cpu_frequency_based_on_load();
    
    // 2. 外设电源管理
    power_manage_peripherals();
    
    // 3. 任务调度优化
    optimize_task_scheduling();
    
    // 4. 睡眠模式
    enter_light_sleep_when_idle();
}

void enter_light_sleep_when_idle(void) {
    // 检查是否所有任务都处于阻塞状态
    if (all_tasks_blocked()) {
        // 配置唤醒源
        configure_wakeup_sources();
        
        // 进入轻度睡眠
        esp_light_sleep_start();
        
        // 唤醒后恢复
        restore_system_state();
    }
}
```

---

**文档版本**: 1.0  
**最后更新**: 2026-03-21  
**设计状态**: 详细设计完成  
**下一步**: 开始编码实现