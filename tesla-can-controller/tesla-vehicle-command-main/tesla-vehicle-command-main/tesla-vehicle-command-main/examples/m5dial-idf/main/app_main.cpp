#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "storage.h"
#include "ble_client.h"
#include "setup_ui.h"
#include "ui_manager.h"
#include "main_menu_page.h"
#include "control_page.h"
#include "g_force_page.h"
#include "setup_page.h"
#include "g_force_data.h"
#include <string>
#include <cstdio>

#include <session.h>
#include <authenticator.h>
#include <shared.h>
#include <universal_message.pb.h>
#include <security.h>
#include <carserver.h>
#include <pb.h>
#include <pb_encode.h>

// New optimization components
#include "ble_reassembler.h"
#include "message_scheduler.h"

static TeslaBLE::Authenticator g_auth;
static TeslaBLE::Session g_session;
static lv_obj_t *g_status_label = nullptr;
static lv_obj_t *g_main_ui = nullptr;
static lv_obj_t *g_setup_ui = nullptr;
static bool g_is_connected = false;
static uint64_t g_last_activity_us = 0;

#ifndef USE_DEEP_SLEEP
#define USE_DEEP_SLEEP 1
#endif

static inline uint64_t now_us() { return esp_timer_get_time(); }
static inline void note_activity() { g_last_activity_us = now_us(); }

// ---- Seat heater helper (nanopb repeated encode callback) ----
typedef struct {
    const CarServer_HvacSeatHeaterActions_HvacSeatHeaterAction *items;
    size_t count;
    size_t index;
} SeatHeaterEncodeCtx;

static bool encode_seat_heater_list(pb_ostream_t *stream, const pb_field_t *field, void * const *arg)
{
    const SeatHeaterEncodeCtx *ctx = (const SeatHeaterEncodeCtx *)(*arg);
    for (size_t i = 0; i < ctx->count; ++i) {
        if (!pb_encode_tag_for_field(stream, field)) return false;
        if (!pb_encode_submessage(stream, CarServer_HvacSeatHeaterActions_HvacSeatHeaterAction_fields, &ctx->items[i])) return false;
    }
    return true;
}

static void send_seat_heater(int pos_tag_value, int level_code)
{
    unsigned char action[96];
    size_t action_len = 0;

    // Build one seat heater action item
    CarServer_HvacSeatHeaterActions_HvacSeatHeaterAction item = CarServer_HvacSeatHeaterActions_HvacSeatHeaterAction_init_default;

    // Level
    // Off/Low/Med/High mapping to nanopb tags
    // 2=OFF,3=LOW,4=MED,5=HIGH
    switch (level_code) {
        case 2: item.which_seat_heater_level = CarServer_HvacSeatHeaterActions_HvacSeatHeaterAction_SEAT_HEATER_OFF_tag; break;
        case 3: item.which_seat_heater_level = CarServer_HvacSeatHeaterActions_HvacSeatHeaterAction_SEAT_HEATER_LOW_tag; break;
        case 4: item.which_seat_heater_level = CarServer_HvacSeatHeaterActions_HvacSeatHeaterAction_SEAT_HEATER_MED_tag; break;
        case 5: item.which_seat_heater_level = CarServer_HvacSeatHeaterActions_HvacSeatHeaterAction_SEAT_HEATER_HIGH_tag; break;
        default: item.which_seat_heater_level = CarServer_HvacSeatHeaterActions_HvacSeatHeaterAction_SEAT_HEATER_UNKNOWN_tag; break;
    }

    // Seat position mapping
    // Use front-left/front-right/rear-left/rear-right mapping via generated tag numbers:
    // 7=FRONT_LEFT, 8=FRONT_RIGHT, 9=REAR_LEFT, 12=REAR_RIGHT
    if (pos_tag_value == 7) item.which_seat_position = CarServer_HvacSeatHeaterActions_HvacSeatHeaterAction_CAR_SEAT_FRONT_LEFT_tag;
    else if (pos_tag_value == 8) item.which_seat_position = CarServer_HvacSeatHeaterActions_HvacSeatHeaterAction_CAR_SEAT_FRONT_RIGHT_tag;
    else if (pos_tag_value == 9) item.which_seat_position = CarServer_HvacSeatHeaterActions_HvacSeatHeaterAction_CAR_SEAT_REAR_LEFT_tag;
    else if (pos_tag_value == 12) item.which_seat_position = CarServer_HvacSeatHeaterActions_HvacSeatHeaterAction_CAR_SEAT_REAR_RIGHT_tag;
    else item.which_seat_position = CarServer_HvacSeatHeaterActions_HvacSeatHeaterAction_CAR_SEAT_UNKNOWN_tag;

    // Build repeated list via callback (single item)
    CarServer_HvacSeatHeaterActions heaters = CarServer_HvacSeatHeaterActions_init_default;
    SeatHeaterEncodeCtx ctx{ &item, 1, 0 };
    heaters.hvacSeatHeaterAction.funcs.encode = &encode_seat_heater_list;
    heaters.hvacSeatHeaterAction.arg = &ctx;

    CarServer_VehicleAction va = CarServer_VehicleAction_init_default;
    va.which_vehicle_action_msg = CarServer_VehicleAction_hvacSeatHeaterActions_tag;
    va.vehicle_action_msg.hvacSeatHeaterActions = heaters;

    g_session.BuildActionMessage(UniversalMessage_Domain_DOMAIN_INFOTAINMENT, &va, action, &action_len);
    if (action_len) {
        unsigned char out[256]; size_t out_len = 0;
        if (g_session.BuildRoutableMessage(UniversalMessage_Domain_DOMAIN_INFOTAINMENT, action, action_len, out, &out_len) == 0) {
            blec::send(out, out_len);
        }
    }
}

// BLE 消息处理 - optimized with reassembler and scheduler
static std::unique_ptr<BLEReassembler> g_ble_reassembler = nullptr;
static std::unique_ptr<MessageScheduler> g_msg_scheduler = nullptr;


/**
 * @brief Process a complete BLE message (reassembled by BLEReassembler)
 */
static void process_complete_ble_message(const uint8_t* data, size_t len) {
    note_activity();
    
    UniversalMessage_RoutableMessage msg = UniversalMessage_RoutableMessage_init_zero;
    TeslaBLE::Common::DecodeRoutableMessage(data, len, &msg);
    
    if (msg.which_payload == UniversalMessage_RoutableMessage_session_info_tag) {
        g_session.UpdateSessionInfo(msg.from_destination.sub_destination.domain,
                                   msg.payload.session_info.bytes,
                                   msg.payload.session_info.size);
        // Persist SessionInfo for fast cold start
        std::string blob((char*)msg.payload.session_info.bytes, msg.payload.session_info.size);
        storage::saveSessionInfo(msg.from_destination.sub_destination.domain, blob);
        if (g_status_label) {
            lv_label_set_text(g_status_label, "已接收 SessionInfo");
        }
    }
    
    // Try to parse vehicle sensor data (for G-force display)
    // Note: Tesla BLE protocol may not directly provide acceleration data
    if (msg.which_payload == UniversalMessage_RoutableMessage_protobuf_message_as_bytes_tag) {
        update_gforce_from_vehicle_data(msg.payload.protobuf_message_as_bytes.bytes,
                                        msg.payload.protobuf_message_as_bytes.size);
    }
    
    if (msg.has_signedMessageStatus) {
        switch (msg.signedMessageStatus.operation_status) {
            case OperationStatus_E_OPERATIONSTATUS_OK:
                if (g_status_label) {
                    lv_label_set_text(g_status_label, "操作成功");
                }
                break;
            case OperationStatus_E_OPERATIONSTATUS_WAIT:
                if (g_status_label) {
                    lv_label_set_text(g_status_label, "请将 NFC 卡靠近车机");
                }
                break;
            case OperationStatus_E_OPERATIONSTATUS_ERROR:
                if (g_status_label) {
                    lv_label_set_text(g_status_label, "操作失败");
                }
                break;
            default:
                if (g_status_label) {
                    lv_label_set_text(g_status_label, "未知状态");
                }
                break;
        }
    }
}

/**
 * @brief BLE data receive callback (called by BLE client)
 */
static void on_ble_rx(const uint8_t *data, size_t len) {
    note_activity();
    
    if (!g_ble_reassembler) {
        // Fallback to direct processing if reassembler not initialized
        process_complete_ble_message(data, len);
        return;
    }
    
    // Feed data to reassembler (handles fragmentation automatically)
    g_ble_reassembler->feed(data, len);
}

/**
 * @brief Determine message priority based on domain and content
 */
static MessagePriority get_message_priority(UniversalMessage_Domain domain) {
    switch (domain) {
        case UniversalMessage_Domain_DOMAIN_VEHICLE_SECURITY:
            return MessagePriority::PRIO_CRITICAL;
        case UniversalMessage_Domain_DOMAIN_INFOTAINMENT:
            return MessagePriority::PRIO_NORMAL;
        default:
            return MessagePriority::PRIO_NORMAL;
    }
}

/**
 * @brief Send BLE message through scheduler
 */
static bool send_ble_message(const uint8_t* data, size_t len) {
    if (!data || len == 0) {
        return false;
    }
    
    // Direct send if scheduler not initialized
    if (!g_msg_scheduler) {
        return blec::send(data, len);
    }
    
    // Use appropriate priority (default to NORMAL if domain unknown)
    // For now use NORMAL, actual implementation should parse domain from message
    MessagePriority priority = MessagePriority::PRIO_NORMAL;
    
    // Try to extract domain from message for better priority assignment
    // This is simplified - actual implementation should decode message header
    if (len >= 4) {
        // Simple heuristic: check if message looks like security command
        // Actual Tesla BLE protocol parsing would be more complex
        if (data[0] == 0x01 || data[1] == 0x01) {  // Very simplified check
            priority = MessagePriority::PRIO_CRITICAL;
        }
    }
    
    return g_msg_scheduler->enqueue(priority, data, len, 5000);  // 5 second timeout
}

static void send_action(UniversalMessage_Domain domain, auto build_fn) {
    unsigned char action[64]; 
    size_t action_len = 0;
    build_fn(action, &action_len);
    if (action_len == 0) return;
    
    unsigned char out[256]; 
    size_t out_len = 0;
    if (g_session.BuildRoutableMessage(domain, action, action_len, out, &out_len) == 0) {
        // Send through scheduler with appropriate priority
        MessagePriority priority = get_message_priority(domain);
        
        if (send_ble_message(out, out_len)) {
            if (g_status_label) {
                lv_label_set_text(g_status_label, "命令已排队…");
            }
        } else {
            if (g_status_label) {
                lv_label_set_text(g_status_label, "发送失败");
            }
        }
    } else {
        if (g_status_label) {
            lv_label_set_text(g_status_label, "会话无效，请先更新会话");
        }
    }
}

// 控制接口函数 - 供control_page使用
extern "C" void vehicle_control_send_action(UniversalMessage_Domain domain, void (*build_fn)(unsigned char*, size_t*)) {
    send_action(domain, build_fn);
}

extern "C" void vehicle_control_send_seat_heater(int pos, int level) {
    send_seat_heater(pos, level);
}

extern "C" void vehicle_control_update_session() {
    unsigned char out[200]; 
    size_t out_len = 0;
    g_session.BuildRequestSessionInfoMessage(UniversalMessage_Domain_DOMAIN_INFOTAINMENT, out, &out_len); 
    send_ble_message(out, out_len);
    g_session.BuildRequestSessionInfoMessage(UniversalMessage_Domain_DOMAIN_VEHICLE_SECURITY, out, &out_len); 
    send_ble_message(out, out_len);
    if (g_status_label) {
        lv_label_set_text(g_status_label, "已请求 SessionInfo…");
    }
}

extern "C" void vehicle_control_whitelist_pair() {
    unsigned char wl[256]; 
    size_t wl_len = 0;
    g_auth.BuildKeyWhitelistMessage(Keys_Role_ROLE_OWNER, wl, &wl_len);
    if (wl_len > 0) { 
        blec::send(wl, wl_len); 
        if (g_status_label) {
            lv_label_set_text(g_status_label, "已发送白名单，请在车端刷卡"); 
        }
    }
}

extern "C" void vehicle_control_set_status(const char* text) {
    if (g_status_label) {
        lv_label_set_text(g_status_label, text);
    }
    // 同时更新control_page的状态
    control_page::g_status_label_ptr = g_status_label;
}

extern "C" void vehicle_control_honk_horn() {
    unsigned char action[64]; 
    size_t action_len = 0;
    CarServer_VehicleAction va = CarServer_VehicleAction_init_default;
    va.which_vehicle_action_msg = CarServer_VehicleAction_vehicleControlHonkHornAction_tag;
    va.vehicle_action_msg.vehicleControlHonkHornAction = CarServer_VehicleControlHonkHornAction_init_default;
    g_session.BuildActionMessage(UniversalMessage_Domain_DOMAIN_VEHICLE_SECURITY, &va, action, &action_len);
    if (action_len) {
        unsigned char out[256]; 
        size_t out_len = 0;
        if (g_session.BuildRoutableMessage(UniversalMessage_Domain_DOMAIN_VEHICLE_SECURITY, action, action_len, out, &out_len) == 0) {
            blec::send(out, out_len);
            vehicle_control_set_status("已发送鸣笛");
        }
    }
}

extern "C" void vehicle_control_flash_lights() {
    unsigned char action[64]; 
    size_t action_len = 0;
    CarServer_VehicleAction va = CarServer_VehicleAction_init_default;
    va.which_vehicle_action_msg = CarServer_VehicleAction_vehicleControlFlashLightsAction_tag;
    va.vehicle_action_msg.vehicleControlFlashLightsAction = CarServer_VehicleControlFlashLightsAction_init_default;
    g_session.BuildActionMessage(UniversalMessage_Domain_DOMAIN_VEHICLE_SECURITY, &va, action, &action_len);
    if (action_len) {
        unsigned char out[256]; 
        size_t out_len = 0;
        if (g_session.BuildRoutableMessage(UniversalMessage_Domain_DOMAIN_VEHICLE_SECURITY, action, action_len, out, &out_len) == 0) {
            blec::send(out, out_len);
            vehicle_control_set_status("已发送闪灯");
        }
    }
}

extern "C" void vehicle_control_steering_heat() {
    send_action(UniversalMessage_Domain_DOMAIN_INFOTAINMENT, [](unsigned char *b, size_t *l){
        CarServer_HvacSteeringWheelHeaterAction action = CarServer_HvacSteeringWheelHeaterAction_init_default;
        action.Level = CarServer_HvacSteeringWheelHeaterAction_Level_Level3; // 最高档
        TeslaBLE::CarServer::BuildActionMessage(&action, b, l);
    });
    vehicle_control_set_status("已发送方向盘加热");
}

extern "C" void vehicle_control_set_charge_limit(int percent) {
    unsigned char action[64]; 
    size_t action_len = 0;
    TeslaBLE::CarServer::SetChargingLimit(percent, action, &action_len);
    if (action_len) {
        unsigned char out[256]; 
        size_t out_len = 0;
        if (g_session.BuildRoutableMessage(UniversalMessage_Domain_DOMAIN_INFOTAINMENT, action, action_len, out, &out_len) == 0) {
            blec::send(out, out_len);
            char msg[64];
            snprintf(msg, sizeof(msg), "已设充电上限%d%%", percent);
            vehicle_control_set_status(msg);
        }
    }
}

extern "C" void vehicle_control_set_charge_amps(int amps) {
    unsigned char action[64]; 
    size_t action_len = 0;
    CarServer_VehicleAction va = CarServer_VehicleAction_init_default;
    va.which_vehicle_action_msg = CarServer_VehicleAction_setChargingAmpsAction_tag;
    va.vehicle_action_msg.setChargingAmpsAction = CarServer_SetChargingAmpsAction_init_default;
    va.vehicle_action_msg.setChargingAmpsAction.charging_amps = amps;
    g_session.BuildActionMessage(UniversalMessage_Domain_DOMAIN_INFOTAINMENT, &va, action, &action_len);
    if (action_len) {
        unsigned char out[256]; 
        size_t out_len = 0;
        if (g_session.BuildRoutableMessage(UniversalMessage_Domain_DOMAIN_INFOTAINMENT, action, action_len, out, &out_len) == 0) {
            blec::send(out, out_len);
            char msg[64];
            snprintf(msg, sizeof(msg), "已设电流%dA", amps);
            vehicle_control_set_status(msg);
        }
    }
}

extern "C" void vehicle_control_climate_keeper_dog() {
    unsigned char action[64]; 
    size_t action_len = 0;
    CarServer_VehicleAction va = CarServer_VehicleAction_init_default;
    va.which_vehicle_action_msg = CarServer_VehicleAction_hvacClimateKeeperAction_tag;
    va.vehicle_action_msg.hvacClimateKeeperAction = CarServer_HvacClimateKeeperAction_init_default;
    va.vehicle_action_msg.hvacClimateKeeperAction.ClimateKeeperAction = CarServer_HvacClimateKeeperAction_ClimateKeeperAction_E_ClimateKeeperAction_Dog;
    g_session.BuildActionMessage(UniversalMessage_Domain_DOMAIN_INFOTAINMENT, &va, action, &action_len);
    if (action_len) {
        unsigned char out[256]; 
        size_t out_len = 0;
        if (g_session.BuildRoutableMessage(UniversalMessage_Domain_DOMAIN_INFOTAINMENT, action, action_len, out, &out_len) == 0) {
            blec::send(out, out_len);
            vehicle_control_set_status("已设气候守护Dog");
        }
    }
}

extern "C" void vehicle_control_climate_keeper_off() {
    unsigned char action[64]; 
    size_t action_len = 0;
    CarServer_VehicleAction va = CarServer_VehicleAction_init_default;
    va.which_vehicle_action_msg = CarServer_VehicleAction_hvacClimateKeeperAction_tag;
    va.vehicle_action_msg.hvacClimateKeeperAction = CarServer_HvacClimateKeeperAction_init_default;
    va.vehicle_action_msg.hvacClimateKeeperAction.ClimateKeeperAction = CarServer_HvacClimateKeeperAction_ClimateKeeperAction_E_ClimateKeeperAction_Off;
    g_session.BuildActionMessage(UniversalMessage_Domain_DOMAIN_INFOTAINMENT, &va, action, &action_len);
    if (action_len) {
        unsigned char out[256]; 
        size_t out_len = 0;
        if (g_session.BuildRoutableMessage(UniversalMessage_Domain_DOMAIN_INFOTAINMENT, action, action_len, out, &out_len) == 0) {
            blec::send(out, out_len);
            vehicle_control_set_status("已关闭气候守护");
        }
    }
}

extern "C" void vehicle_control_front_trunk() {
    unsigned char action[64]; 
    size_t action_len = 0;
    CarServer_VehicleAction va = CarServer_VehicleAction_init_default;
    va.which_vehicle_action_msg = CarServer_VehicleAction_vehicleControlFrontTrunkAction_tag;
    va.vehicle_action_msg.vehicleControlFrontTrunkAction = CarServer_VehicleControlFrontTrunkAction_init_default;
    g_session.BuildActionMessage(UniversalMessage_Domain_DOMAIN_VEHICLE_SECURITY, &va, action, &action_len);
    if (action_len) {
        unsigned char out[256]; 
        size_t out_len = 0;
        if (g_session.BuildRoutableMessage(UniversalMessage_Domain_DOMAIN_VEHICLE_SECURITY, action, action_len, out, &out_len) == 0) {
            blec::send(out, out_len);
            vehicle_control_set_status("已发送前备箱");
        }
    }
}

static void create_main_ui() {
    g_main_ui = lv_obj_create(lv_scr_act());
    lv_obj_set_size(g_main_ui, LV_PCT(100), LV_PCT(100));
    
    // 状态标签
    g_status_label = lv_label_create(g_main_ui);
    lv_label_set_text(g_status_label, "等待连接…");
    lv_obj_align(g_status_label, LV_ALIGN_TOP_MID, 0, 10);
    
    // 控制按钮
    auto mk_btn = [&](const char *txt, lv_coord_t y, lv_event_cb_t cb) {
        lv_obj_t *btn = lv_btn_create(g_main_ui);
        lv_obj_set_size(btn, 200, 40);
        lv_obj_align(btn, LV_ALIGN_TOP_MID, 0, y);
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
        lv_obj_t *l = lv_label_create(btn); 
        lv_label_set_text(l, txt); 
        lv_obj_center(l);
        return btn;
    };
    
    mk_btn("更新会话", 40, [](lv_event_t *e) {
        (void)e;
        unsigned char out[200]; 
        size_t out_len = 0;
        g_session.BuildRequestSessionInfoMessage(UniversalMessage_Domain_DOMAIN_INFOTAINMENT, out, &out_len); 
        blec::send(out, out_len);
        g_session.BuildRequestSessionInfoMessage(UniversalMessage_Domain_DOMAIN_VEHICLE_SECURITY, out, &out_len); 
        blec::send(out, out_len);
        lv_label_set_text(g_status_label, "已请求 SessionInfo…");
    });
    
    mk_btn("白名单配对", 90, [](lv_event_t *e) {
        (void)e;
        unsigned char wl[256]; 
        size_t wl_len = 0;
        g_auth.BuildKeyWhitelistMessage(Keys_Role_ROLE_OWNER, wl, &wl_len);
        if (wl_len > 0) { 
            blec::send(wl, wl_len); 
            lv_label_set_text(g_status_label, "已发送白名单，请在车端刷卡"); 
        }
    });
    
    mk_btn("解锁", 140, [](lv_event_t *e) {
        (void)e; 
        send_action(UniversalMessage_Domain_DOMAIN_VEHICLE_SECURITY, [](unsigned char *b, size_t *l){ TeslaBLE::Security::Unlock(b, l); });
    });
    
    mk_btn("上锁", 190, [](lv_event_t *e) {
        (void)e; 
        send_action(UniversalMessage_Domain_DOMAIN_VEHICLE_SECURITY, [](unsigned char *b, size_t *l){ TeslaBLE::Security::Lock(b, l); });
    });
    
    mk_btn("空调开", 240, [](lv_event_t *e) {
        (void)e; 
        send_action(UniversalMessage_Domain_DOMAIN_INFOTAINMENT, [](unsigned char *b, size_t *l){ TeslaBLE::CarServer::TurnOnClimate(b, l); });
    });
    
    mk_btn("空调关", 290, [](lv_event_t *e) {
        (void)e; 
        send_action(UniversalMessage_Domain_DOMAIN_INFOTAINMENT, [](unsigned char *b, size_t *l){ TeslaBLE::CarServer::TurnOffClimate(b, l); });
    });
    
    mk_btn("下一曲", 340, [](lv_event_t *e) {
        (void)e; 
        send_action(UniversalMessage_Domain_DOMAIN_INFOTAINMENT, [](unsigned char *b, size_t *l){ TeslaBLE::CarServer::NextMediaTrack(b, l); });
    });
    
    mk_btn("鸣笛", 390, [](lv_event_t *e) {
        (void)e;
        unsigned char action[64]; size_t action_len = 0;
        CarServer_VehicleAction va = CarServer_VehicleAction_init_default;
        va.which_vehicle_action_msg = CarServer_VehicleAction_vehicleControlHonkHornAction_tag;
        va.vehicle_action_msg.vehicleControlHonkHornAction = CarServer_VehicleControlHonkHornAction_init_default;
        g_session.BuildActionMessage(UniversalMessage_Domain_DOMAIN_VEHICLE_SECURITY, &va, action, &action_len);
        if (action_len) {
            unsigned char out[256]; size_t out_len = 0;
            if (g_session.BuildRoutableMessage(UniversalMessage_Domain_DOMAIN_VEHICLE_SECURITY, action, action_len, out, &out_len) == 0) {
                blec::send(out, out_len);
                lv_label_set_text(g_status_label, "已发送鸣笛");
            }
        }
    });

    // 方向盘加热按钮（靠前区域）
    mk_btn("方向盘加热", 440, [](lv_event_t *e) {
        (void)e;
        send_action(UniversalMessage_Domain_DOMAIN_INFOTAINMENT, [](unsigned char *b, size_t *l){
            CarServer_HvacSteeringWheelHeaterAction action = CarServer_HvacSteeringWheelHeaterAction_init_default;
            action.Level = CarServer_HvacSteeringWheelHeaterAction_Level_Level3; // 最高档
            TeslaBLE::CarServer::BuildActionMessage(&action, b, l);
        });
    });

    mk_btn("闪灯", 490, [](lv_event_t *e) {
        (void)e;
        unsigned char action[64]; size_t action_len = 0;
        CarServer_VehicleAction va = CarServer_VehicleAction_init_default;
        va.which_vehicle_action_msg = CarServer_VehicleAction_vehicleControlFlashLightsAction_tag;
        va.vehicle_action_msg.vehicleControlFlashLightsAction = CarServer_VehicleControlFlashLightsAction_init_default;
        g_session.BuildActionMessage(UniversalMessage_Domain_DOMAIN_VEHICLE_SECURITY, &va, action, &action_len);
        if (action_len) {
            unsigned char out[256]; size_t out_len = 0;
            if (g_session.BuildRoutableMessage(UniversalMessage_Domain_DOMAIN_VEHICLE_SECURITY, action, action_len, out, &out_len) == 0) {
                blec::send(out, out_len);
                lv_label_set_text(g_status_label, "已发送闪灯");
            }
        }
    });

    mk_btn("开始充电", 540, [](lv_event_t *e) {
        (void)e;
        send_action(UniversalMessage_Domain_DOMAIN_INFOTAINMENT, [](unsigned char *b, size_t *l){ TeslaBLE::CarServer::StartCharging(b, l); });
    });

    mk_btn("停止充电", 590, [](lv_event_t *e) {
        (void)e;
        send_action(UniversalMessage_Domain_DOMAIN_INFOTAINMENT, [](unsigned char *b, size_t *l){ TeslaBLE::CarServer::StopCharging(b, l); });
    });

    mk_btn("开充电口", 640, [](lv_event_t *e) {
        (void)e;
        send_action(UniversalMessage_Domain_DOMAIN_INFOTAINMENT, [](unsigned char *b, size_t *l){ TeslaBLE::CarServer::OpenChargePort(b, l); });
    });

    mk_btn("关充电口", 690, [](lv_event_t *e) {
        (void)e;
        send_action(UniversalMessage_Domain_DOMAIN_INFOTAINMENT, [](unsigned char *b, size_t *l){ TeslaBLE::CarServer::CloseChargePort(b, l); });
    });

    mk_btn("设限80%", 740, [](lv_event_t *e) {
        (void)e;
        unsigned char action[64]; size_t action_len = 0;
        TeslaBLE::CarServer::SetChargingLimit(80, action, &action_len);
        if (action_len) {
            unsigned char out[256]; size_t out_len = 0;
            if (g_session.BuildRoutableMessage(UniversalMessage_Domain_DOMAIN_INFOTAINMENT, action, action_len, out, &out_len) == 0) {
                blec::send(out, out_len);
                lv_label_set_text(g_status_label, "已设充电上限80%");
            }
        }
    });

    mk_btn("电流16A", 790, [](lv_event_t *e) {
        (void)e;
        unsigned char action[64]; size_t action_len = 0;
        CarServer_VehicleAction va = CarServer_VehicleAction_init_default;
        va.which_vehicle_action_msg = CarServer_VehicleAction_setChargingAmpsAction_tag;
        va.vehicle_action_msg.setChargingAmpsAction = CarServer_SetChargingAmpsAction_init_default;
        va.vehicle_action_msg.setChargingAmpsAction.charging_amps = 16;
        g_session.BuildActionMessage(UniversalMessage_Domain_DOMAIN_INFOTAINMENT, &va, action, &action_len);
        if (action_len) {
            unsigned char out[256]; size_t out_len = 0;
            if (g_session.BuildRoutableMessage(UniversalMessage_Domain_DOMAIN_INFOTAINMENT, action, action_len, out, &out_len) == 0) {
                blec::send(out, out_len);
                lv_label_set_text(g_status_label, "已设电流16A");
            }
        }
    });

    mk_btn("气候守护Dog", 840, [](lv_event_t *e) {
        (void)e;
        unsigned char action[64]; size_t action_len = 0;
        CarServer_VehicleAction va = CarServer_VehicleAction_init_default;
        va.which_vehicle_action_msg = CarServer_VehicleAction_hvacClimateKeeperAction_tag;
        va.vehicle_action_msg.hvacClimateKeeperAction = CarServer_HvacClimateKeeperAction_init_default;
        va.vehicle_action_msg.hvacClimateKeeperAction.ClimateKeeperAction = CarServer_HvacClimateKeeperAction_ClimateKeeperAction_E_ClimateKeeperAction_Dog;
        g_session.BuildActionMessage(UniversalMessage_Domain_DOMAIN_INFOTAINMENT, &va, action, &action_len);
        if (action_len) {
            unsigned char out[256]; size_t out_len = 0;
            if (g_session.BuildRoutableMessage(UniversalMessage_Domain_DOMAIN_INFOTAINMENT, action, action_len, out, &out_len) == 0) {
                blec::send(out, out_len);
                lv_label_set_text(g_status_label, "已设气候守护Dog");
            }
        }
    });

    mk_btn("气候守护关", 890, [](lv_event_t *e) {
        (void)e;
        unsigned char action[64]; size_t action_len = 0;
        CarServer_VehicleAction va = CarServer_VehicleAction_init_default;
        va.which_vehicle_action_msg = CarServer_VehicleAction_hvacClimateKeeperAction_tag;
        va.vehicle_action_msg.hvacClimateKeeperAction = CarServer_HvacClimateKeeperAction_init_default;
        va.vehicle_action_msg.hvacClimateKeeperAction.ClimateKeeperAction = CarServer_HvacClimateKeeperAction_ClimateKeeperAction_E_ClimateKeeperAction_Off;
        g_session.BuildActionMessage(UniversalMessage_Domain_DOMAIN_INFOTAINMENT, &va, action, &action_len);
        if (action_len) {
            unsigned char out[256]; size_t out_len = 0;
            if (g_session.BuildRoutableMessage(UniversalMessage_Domain_DOMAIN_INFOTAINMENT, action, action_len, out, &out_len) == 0) {
                blec::send(out, out_len);
                lv_label_set_text(g_status_label, "已关闭气候守护");
            }
        }
    });

    mk_btn("定时充电22:00", 940, [](lv_event_t *e) {
        (void)e;
        unsigned char action[64]; size_t action_len = 0;
        CarServer_VehicleAction va = CarServer_VehicleAction_init_default;
        va.which_vehicle_action_msg = CarServer_VehicleAction_scheduledChargingAction_tag;
        va.vehicle_action_msg.scheduledChargingAction = CarServer_ScheduledChargingAction_init_default;
        va.vehicle_action_msg.scheduledChargingAction.enabled = true;
        va.vehicle_action_msg.scheduledChargingAction.charging_time = 1320; // 22:00 -> 22*60
        g_session.BuildActionMessage(UniversalMessage_Domain_DOMAIN_INFOTAINMENT, &va, action, &action_len);
        if (action_len) {
            unsigned char out[256]; size_t out_len = 0;
            if (g_session.BuildRoutableMessage(UniversalMessage_Domain_DOMAIN_INFOTAINMENT, action, action_len, out, &out_len) == 0) {
                blec::send(out, out_len);
                lv_label_set_text(g_status_label, "已设定时充电22:00");
            }
        }
    });

    mk_btn("定时出发07:30", 990, [](lv_event_t *e) {
        (void)e;
        unsigned char action[64]; size_t action_len = 0;
        CarServer_VehicleAction va = CarServer_VehicleAction_init_default;
        va.which_vehicle_action_msg = CarServer_VehicleAction_scheduledDepartureAction_tag;
        va.vehicle_action_msg.scheduledDepartureAction = CarServer_ScheduledDepartureAction_init_default;
        va.vehicle_action_msg.scheduledDepartureAction.enabled = true;
        va.vehicle_action_msg.scheduledDepartureAction.departure_time = 450; // 7:30 -> 7*60+30
        g_session.BuildActionMessage(UniversalMessage_Domain_DOMAIN_INFOTAINMENT, &va, action, &action_len);
        if (action_len) {
            unsigned char out[256]; size_t out_len = 0;
            if (g_session.BuildRoutableMessage(UniversalMessage_Domain_DOMAIN_INFOTAINMENT, action, action_len, out, &out_len) == 0) {
                blec::send(out, out_len);
                lv_label_set_text(g_status_label, "已设定时出发07:30");
            }
        }
    });


    mk_btn("重新设置", 1040, [](lv_event_t *e) {
        (void)e;
        lv_obj_add_flag(g_main_ui, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(g_setup_ui, LV_OBJ_FLAG_HIDDEN);
        setup_ui::show_vin_input();
    });

    // 座椅加热菜单按钮
    mk_btn("座椅加热", 1090, [](lv_event_t *e) {
        (void)e;
        // 创建座椅加热子菜单
        lv_obj_t *seat_menu = lv_obj_create(lv_scr_act());
        lv_obj_set_size(seat_menu, 240, 240);
        lv_obj_center(seat_menu);
        lv_obj_set_style_bg_color(seat_menu, lv_color_hex(0x000000), 0);
        lv_obj_set_style_border_width(seat_menu, 2, 0);
        lv_obj_set_style_border_color(seat_menu, lv_color_hex(0x00ff00), 0);
        
        // 标题
        lv_obj_t *title = lv_label_create(seat_menu);
        lv_label_set_text(title, "座椅加热控制");
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);
        lv_obj_set_style_text_color(title, lv_color_hex(0x00ff00), 0);
        
        // 前左座椅
        lv_obj_t *fl_label = lv_label_create(seat_menu);
        lv_label_set_text(fl_label, "前左:");
        lv_obj_align(fl_label, LV_ALIGN_TOP_LEFT, 10, 40);
        lv_obj_set_style_text_color(fl_label, lv_color_hex(0xffffff), 0);
        
        lv_obj_t *fl_off = lv_btn_create(seat_menu);
        lv_obj_set_size(fl_off, 40, 25);
        lv_obj_align(fl_off, LV_ALIGN_TOP_LEFT, 50, 40);
        lv_obj_t *fl_off_label = lv_label_create(fl_off);
        lv_label_set_text(fl_off_label, "关");
        lv_obj_center(fl_off_label);
        lv_obj_add_event_cb(fl_off, [](lv_event_t *e){ (void)e; send_seat_heater(7, 2); }, LV_EVENT_CLICKED, nullptr);
        
        lv_obj_t *fl_low = lv_btn_create(seat_menu);
        lv_obj_set_size(fl_low, 40, 25);
        lv_obj_align(fl_low, LV_ALIGN_TOP_LEFT, 95, 40);
        lv_obj_t *fl_low_label = lv_label_create(fl_low);
        lv_label_set_text(fl_low_label, "低");
        lv_obj_center(fl_low_label);
        lv_obj_add_event_cb(fl_low, [](lv_event_t *e){ (void)e; send_seat_heater(7, 3); }, LV_EVENT_CLICKED, nullptr);
        
        lv_obj_t *fl_med = lv_btn_create(seat_menu);
        lv_obj_set_size(fl_med, 40, 25);
        lv_obj_align(fl_med, LV_ALIGN_TOP_LEFT, 140, 40);
        lv_obj_t *fl_med_label = lv_label_create(fl_med);
        lv_label_set_text(fl_med_label, "中");
        lv_obj_center(fl_med_label);
        lv_obj_add_event_cb(fl_med, [](lv_event_t *e){ (void)e; send_seat_heater(7, 4); }, LV_EVENT_CLICKED, nullptr);
        
        lv_obj_t *fl_high = lv_btn_create(seat_menu);
        lv_obj_set_size(fl_high, 40, 25);
        lv_obj_align(fl_high, LV_ALIGN_TOP_LEFT, 185, 40);
        lv_obj_t *fl_high_label = lv_label_create(fl_high);
        lv_label_set_text(fl_high_label, "高");
        lv_obj_center(fl_high_label);
        lv_obj_add_event_cb(fl_high, [](lv_event_t *e){ (void)e; send_seat_heater(7, 5); }, LV_EVENT_CLICKED, nullptr);
        
        // 前右座椅
        lv_obj_t *fr_label = lv_label_create(seat_menu);
        lv_label_set_text(fr_label, "前右:");
        lv_obj_align(fr_label, LV_ALIGN_TOP_LEFT, 10, 75);
        lv_obj_set_style_text_color(fr_label, lv_color_hex(0xffffff), 0);
        
        lv_obj_t *fr_off = lv_btn_create(seat_menu);
        lv_obj_set_size(fr_off, 40, 25);
        lv_obj_align(fr_off, LV_ALIGN_TOP_LEFT, 50, 75);
        lv_obj_t *fr_off_label = lv_label_create(fr_off);
        lv_label_set_text(fr_off_label, "关");
        lv_obj_center(fr_off_label);
        lv_obj_add_event_cb(fr_off, [](lv_event_t *e){ (void)e; send_seat_heater(8, 2); }, LV_EVENT_CLICKED, nullptr);
        
        lv_obj_t *fr_low = lv_btn_create(seat_menu);
        lv_obj_set_size(fr_low, 40, 25);
        lv_obj_align(fr_low, LV_ALIGN_TOP_LEFT, 95, 75);
        lv_obj_t *fr_low_label = lv_label_create(fr_low);
        lv_label_set_text(fr_low_label, "低");
        lv_obj_center(fr_low_label);
        lv_obj_add_event_cb(fr_low, [](lv_event_t *e){ (void)e; send_seat_heater(8, 3); }, LV_EVENT_CLICKED, nullptr);
        
        lv_obj_t *fr_med = lv_btn_create(seat_menu);
        lv_obj_set_size(fr_med, 40, 25);
        lv_obj_align(fr_med, LV_ALIGN_TOP_LEFT, 140, 75);
        lv_obj_t *fr_med_label = lv_label_create(fr_med);
        lv_label_set_text(fr_med_label, "中");
        lv_obj_center(fr_med_label);
        lv_obj_add_event_cb(fr_med, [](lv_event_t *e){ (void)e; send_seat_heater(8, 4); }, LV_EVENT_CLICKED, nullptr);
        
        lv_obj_t *fr_high = lv_btn_create(seat_menu);
        lv_obj_set_size(fr_high, 40, 25);
        lv_obj_align(fr_high, LV_ALIGN_TOP_LEFT, 185, 75);
        lv_obj_t *fr_high_label = lv_label_create(fr_high);
        lv_label_set_text(fr_high_label, "高");
        lv_obj_center(fr_high_label);
        lv_obj_add_event_cb(fr_high, [](lv_event_t *e){ (void)e; send_seat_heater(8, 5); }, LV_EVENT_CLICKED, nullptr);
        
        // 后左座椅
        lv_obj_t *rl_label = lv_label_create(seat_menu);
        lv_label_set_text(rl_label, "后左:");
        lv_obj_align(rl_label, LV_ALIGN_TOP_LEFT, 10, 110);
        lv_obj_set_style_text_color(rl_label, lv_color_hex(0xffffff), 0);
        
        lv_obj_t *rl_off = lv_btn_create(seat_menu);
        lv_obj_set_size(rl_off, 40, 25);
        lv_obj_align(rl_off, LV_ALIGN_TOP_LEFT, 50, 110);
        lv_obj_t *rl_off_label = lv_label_create(rl_off);
        lv_label_set_text(rl_off_label, "关");
        lv_obj_center(rl_off_label);
        lv_obj_add_event_cb(rl_off, [](lv_event_t *e){ (void)e; send_seat_heater(9, 2); }, LV_EVENT_CLICKED, nullptr);
        
        lv_obj_t *rl_low = lv_btn_create(seat_menu);
        lv_obj_set_size(rl_low, 40, 25);
        lv_obj_align(rl_low, LV_ALIGN_TOP_LEFT, 95, 110);
        lv_obj_t *rl_low_label = lv_label_create(rl_low);
        lv_label_set_text(rl_low_label, "低");
        lv_obj_center(rl_low_label);
        lv_obj_add_event_cb(rl_low, [](lv_event_t *e){ (void)e; send_seat_heater(9, 3); }, LV_EVENT_CLICKED, nullptr);
        
        lv_obj_t *rl_med = lv_btn_create(seat_menu);
        lv_obj_set_size(rl_med, 40, 25);
        lv_obj_align(rl_med, LV_ALIGN_TOP_LEFT, 140, 110);
        lv_obj_t *rl_med_label = lv_label_create(rl_med);
        lv_label_set_text(rl_med_label, "中");
        lv_obj_center(rl_med_label);
        lv_obj_add_event_cb(rl_med, [](lv_event_t *e){ (void)e; send_seat_heater(9, 4); }, LV_EVENT_CLICKED, nullptr);
        
        lv_obj_t *rl_high = lv_btn_create(seat_menu);
        lv_obj_set_size(rl_high, 40, 25);
        lv_obj_align(rl_high, LV_ALIGN_TOP_LEFT, 185, 110);
        lv_obj_t *rl_high_label = lv_label_create(rl_high);
        lv_label_set_text(rl_high_label, "高");
        lv_obj_center(rl_high_label);
        lv_obj_add_event_cb(rl_high, [](lv_event_t *e){ (void)e; send_seat_heater(9, 5); }, LV_EVENT_CLICKED, nullptr);
        
        // 后右座椅
        lv_obj_t *rr_label = lv_label_create(seat_menu);
        lv_label_set_text(rr_label, "后右:");
        lv_obj_align(rr_label, LV_ALIGN_TOP_LEFT, 10, 145);
        lv_obj_set_style_text_color(rr_label, lv_color_hex(0xffffff), 0);
        
        lv_obj_t *rr_off = lv_btn_create(seat_menu);
        lv_obj_set_size(rr_off, 40, 25);
        lv_obj_align(rr_off, LV_ALIGN_TOP_LEFT, 50, 145);
        lv_obj_t *rr_off_label = lv_label_create(rr_off);
        lv_label_set_text(rr_off_label, "关");
        lv_obj_center(rr_off_label);
        lv_obj_add_event_cb(rr_off, [](lv_event_t *e){ (void)e; send_seat_heater(12, 2); }, LV_EVENT_CLICKED, nullptr);
        
        lv_obj_t *rr_low = lv_btn_create(seat_menu);
        lv_obj_set_size(rr_low, 40, 25);
        lv_obj_align(rr_low, LV_ALIGN_TOP_LEFT, 95, 145);
        lv_obj_t *rr_low_label = lv_label_create(rr_low);
        lv_label_set_text(rr_low_label, "低");
        lv_obj_center(rr_low_label);
        lv_obj_add_event_cb(rr_low, [](lv_event_t *e){ (void)e; send_seat_heater(12, 3); }, LV_EVENT_CLICKED, nullptr);
        
        lv_obj_t *rr_med = lv_btn_create(seat_menu);
        lv_obj_set_size(rr_med, 40, 25);
        lv_obj_align(rr_med, LV_ALIGN_TOP_LEFT, 140, 145);
        lv_obj_t *rr_med_label = lv_label_create(rr_med);
        lv_label_set_text(rr_med_label, "中");
        lv_obj_center(rr_med_label);
        lv_obj_add_event_cb(rr_med, [](lv_event_t *e){ (void)e; send_seat_heater(12, 4); }, LV_EVENT_CLICKED, nullptr);
        
        lv_obj_t *rr_high = lv_btn_create(seat_menu);
        lv_obj_set_size(rr_high, 40, 25);
        lv_obj_align(rr_high, LV_ALIGN_TOP_LEFT, 185, 145);
        lv_obj_t *rr_high_label = lv_label_create(rr_high);
        lv_label_set_text(rr_high_label, "高");
        lv_obj_center(rr_high_label);
        lv_obj_add_event_cb(rr_high, [](lv_event_t *e){ (void)e; send_seat_heater(12, 5); }, LV_EVENT_CLICKED, nullptr);
        
        // 关闭按钮
        lv_obj_t *close_btn = lv_btn_create(seat_menu);
        lv_obj_set_size(close_btn, 80, 30);
        lv_obj_align(close_btn, LV_ALIGN_BOTTOM_MID, 0, -10);
        lv_obj_t *close_label = lv_label_create(close_btn);
        lv_label_set_text(close_label, "关闭");
        lv_obj_center(close_label);
        lv_obj_add_event_cb(close_btn, [](lv_event_t *e) {
            (void)e;
            lv_obj_del(lv_obj_get_parent(lv_event_get_target(e)));
        }, LV_EVENT_CLICKED, nullptr);
    });
}

static void create_setup_ui() {
    g_setup_ui = lv_obj_create(lv_scr_act());
    lv_obj_set_size(g_setup_ui, LV_PCT(100), LV_PCT(100));
    lv_obj_add_flag(g_setup_ui, LV_OBJ_FLAG_HIDDEN);
    
    setup_ui::create_ui(g_setup_ui);
}

static void encoder_callback(int16_t delta) {
    note_activity();
    ui_manager::PageManager::instance().on_encoder(delta);
}

static void encoder_press_callback() {
    note_activity();
    ui_manager::PageManager::instance().on_encoder_press();
}

static void button_callback() {
    note_activity();
    ui_manager::PageManager::instance().on_button();
}

extern "C" void app_main(void) {
    const bsp_display_config_t disp_cfg = {
        .max_transfer_sz = 64 * 1024,
        .flags = {
            .buff_dma = 1,
        },
    };
    bsp_display_start_with_config(&disp_cfg);
    bsp_display_backlight_on();

    storage::init();
    
    // Initialize optimization components
    ESP_LOGI("app_main", "Initializing BLE optimization components...");
    
    // Create BLE reassembler with 2-second timeout
    g_ble_reassembler = std::make_unique<BLEReassembler>(
        [](const uint8_t* data, size_t len) {
            process_complete_ble_message(data, len);
        },
        2000  // 2 second timeout
    );
    
    // Create message scheduler with direct BLE send callback
    g_msg_scheduler = std::make_unique<MessageScheduler>(
        [](const uint8_t* data, size_t len) -> bool {
            return blec::send(data, len);
        },
        100  // Max queue size
    );
    
    // Start scheduler task on core 1 (leaving core 0 for UI)
    if (!g_msg_scheduler->start(4096, 5, 1)) {
        ESP_LOGW("app_main", "Failed to start message scheduler, using direct send");
    } else {
        ESP_LOGI("app_main", "Message scheduler started successfully");
    }
    
    note_activity();
    
    // 初始化 BSP 输入
    bsp_btn_register_callback(BSP_BUTTON_MAIN, BUTTON_LONG_PRESS, [](void *arg) {
        (void)arg;
        button_callback();
    }, NULL);
    
    bsp_btn_register_callback(BSP_BUTTON_MAIN, BUTTON_SHORT_PRESS, [](void *arg) {
        (void)arg;
        button_callback();
    }, NULL);
    
    // 初始化旋钮（根据 esp-bsp m5dial 文档）
    bsp_encoder_init();
    bsp_encoder_register_callback(encoder_callback, encoder_press_callback);

    // 初始化 Setup UI
    setup_ui::init();
    
    // 初始化UI管理器
    ui_manager::PageManager& pm = ui_manager::PageManager::instance();
    pm.init();
    
    // 创建页面实例
    static main_menu::MainMenuPage main_menu_page;
    static control_page::ControlPage control_page;
    static g_force_page::GForcePage g_force_page;
    static setup_page::SetupPage setup_page;
    
    // 设置全局指针供control_page使用
    control_page::g_auth_ptr = &g_auth;
    control_page::g_session_ptr = &g_session;
    
    // 设置G值表页面指针
    set_gforce_page_ptr(&g_force_page);
    
    // 注册页面
    pm.register_page(ui_manager::PageType::MAIN_MENU, &main_menu_page);
    pm.register_page(ui_manager::PageType::CONTROL, &control_page);
    pm.register_page(ui_manager::PageType::G_FORCE, &g_force_page);
    pm.register_page(ui_manager::PageType::SETUP, &setup_page);
    
    // 检查是否需要设置
    std::string vin, mac;
    bool needs_setup = !storage::loadVin(vin) || !storage::loadMac(mac);
    
    if (needs_setup) {
        pm.switch_to(ui_manager::PageType::SETUP);
    } else {
        pm.switch_to(ui_manager::PageType::MAIN_MENU);
        
        // 初始化 Tesla 协议
        std::string pem;
        if (storage::loadPrivateKey(pem)) {
            g_auth.LoadPrivateKey((const uint8_t*)pem.data(), pem.size());
        } else {
            if (g_auth.CreatePrivateKey() == 0) {
                unsigned char keybuf[256]; 
                size_t keylen = 0;
                if (g_auth.GetPrivateKey(keybuf, sizeof keybuf, &keylen) == 0) {
                    storage::savePrivateKey(std::string((char*)keybuf, keylen));
                }
            }
        }
        
        g_session.SetVIN((unsigned char*)vin.c_str());
        g_session.GenerateRoutingAddress();
        g_session.LoadAuthenticator(&g_auth);
        // 试图恢复已保存的 SessionInfo
        std::string sess_inf, sess_vs;
        if (storage::loadSessionInfo(UniversalMessage_Domain_DOMAIN_INFOTAINMENT, sess_inf)) {
            g_session.UpdateSessionInfo(UniversalMessage_Domain_DOMAIN_INFOTAINMENT,
                                        (unsigned char*)sess_inf.data(), sess_inf.size());
        }
        if (storage::loadSessionInfo(UniversalMessage_Domain_DOMAIN_VEHICLE_SECURITY, sess_vs)) {
            g_session.UpdateSessionInfo(UniversalMessage_Domain_DOMAIN_VEHICLE_SECURITY,
                                        (unsigned char*)sess_vs.data(), sess_vs.size());
        }
        
        // 尝试连接车辆
        if (storage::loadMac(mac)) {
            lv_label_set_text(g_status_label, "正在连接车辆…");
            g_is_connected = blec::start(mac, on_ble_rx);
            if (g_is_connected) {
                lv_label_set_text(g_status_label, "连接成功，请求 SessionInfo…");
                unsigned char out[200]; 
                size_t out_len = 0;
                g_session.BuildRequestSessionInfoMessage(UniversalMessage_Domain_DOMAIN_INFOTAINMENT, out, &out_len); 
                blec::send(out, out_len);
                g_session.BuildRequestSessionInfoMessage(UniversalMessage_Domain_DOMAIN_VEHICLE_SECURITY, out, &out_len); 
                blec::send(out, out_len);
            } else {
                lv_label_set_text(g_status_label, "连接失败，请检查 MAC");
            }
        }
    }

    // 主循环
    const uint64_t kDimMs = 30000ULL;
    const uint64_t kSleepMs = 120000ULL;
    while (true) {
        // 更新UI管理器（用于页面动画等）
        pm.update();
        
        // 检查设置是否完成（如果当前在设置页面）
        if (pm.current_page() == ui_manager::PageType::SETUP && setup_ui::is_setup_complete()) {
            // 重新初始化 Tesla 协议
            std::string new_vin = setup_ui::get_vin();
            std::string new_mac = setup_ui::get_mac();
            
            std::string pem;
            if (storage::loadPrivateKey(pem)) {
                g_auth.LoadPrivateKey((const uint8_t*)pem.data(), pem.size());
            } else {
                if (g_auth.CreatePrivateKey() == 0) {
                    unsigned char keybuf[256]; 
                    size_t keylen = 0;
                    if (g_auth.GetPrivateKey(keybuf, sizeof keybuf, &keylen) == 0) {
                        storage::savePrivateKey(std::string((char*)keybuf, keylen));
                    }
                }
            }
            
            g_session.SetVIN((unsigned char*)new_vin.c_str());
            g_session.GenerateRoutingAddress();
            g_session.LoadAuthenticator(&g_auth);
            // 试图恢复已保存的 SessionInfo
            std::string sess_inf2, sess_vs2;
            if (storage::loadSessionInfo(UniversalMessage_Domain_DOMAIN_INFOTAINMENT, sess_inf2)) {
                g_session.UpdateSessionInfo(UniversalMessage_Domain_DOMAIN_INFOTAINMENT,
                                            (unsigned char*)sess_inf2.data(), sess_inf2.size());
            }
            if (storage::loadSessionInfo(UniversalMessage_Domain_DOMAIN_VEHICLE_SECURITY, sess_vs2)) {
                g_session.UpdateSessionInfo(UniversalMessage_Domain_DOMAIN_VEHICLE_SECURITY,
                                            (unsigned char*)sess_vs2.data(), sess_vs2.size());
            }
            
            // 尝试连接
            if (g_status_label) {
                lv_label_set_text(g_status_label, "正在连接车辆…");
            }
            g_is_connected = blec::start(new_mac, on_ble_rx);
            if (g_is_connected) {
                if (g_status_label) {
                    lv_label_set_text(g_status_label, "连接成功，请求 SessionInfo…");
                }
                unsigned char out[200]; 
                size_t out_len = 0;
                g_session.BuildRequestSessionInfoMessage(UniversalMessage_Domain_DOMAIN_INFOTAINMENT, out, &out_len); 
                blec::send(out, out_len);
                g_session.BuildRequestSessionInfoMessage(UniversalMessage_Domain_DOMAIN_VEHICLE_SECURITY, out, &out_len); 
                blec::send(out, out_len);
            } else {
                if (g_status_label) {
                    lv_label_set_text(g_status_label, "连接失败，请检查 MAC");
                }
            }
            
            // 切换到主菜单
            pm.switch_to(ui_manager::PageType::MAIN_MENU);
        }
        
        // 省电策略：空闲30s 关闭背光；空闲120s 进入深度睡眠(可选)
        uint64_t idle_ms = (now_us() - g_last_activity_us) / 1000ULL;
        if (idle_ms > kDimMs) {
            bsp_display_backlight_off();
        } else {
            bsp_display_backlight_on();
        }

#if USE_DEEP_SLEEP
        if (idle_ms > kSleepMs) {
            // 简化：任意 GPIO 唤醒或定时唤醒(这里配置定时 1s 仅示例，可改为按钮 GPIO)
            esp_sleep_enable_timer_wakeup(1000ULL * 1000ULL * 3600ULL); // 1h 最大占位
            esp_deep_sleep_start();
        }
#endif
        
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
