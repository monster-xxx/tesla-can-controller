#include "ble_reassembler.h"
#include "esp_log.h"
#include <cstring>

static const char* TAG = "ble_reassembler";

// Tesla BLE protocol uses 2-byte length prefix
static constexpr size_t LENGTH_PREFIX_SIZE = 2;

BLEReassembler::BLEReassembler(MessageCallback callback, uint32_t timeout_ms)
    : callback_(std::move(callback)), timeout_ms_(timeout_ms) {
    
    mutex_ = xSemaphoreCreateMutex();
    if (!mutex_) {
        ESP_LOGE(TAG, "Failed to create mutex");
    }
    
    // Create timeout timer
    esp_timer_create_args_t timer_args = {
        .callback = &BLEReassembler::timeout_callback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "ble_reassemble_timeout"
    };
    
    esp_err_t err = esp_timer_create(&timer_args, &timeout_timer_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create timeout timer: %s", esp_err_to_name(err));
    }
    
    ESP_LOGI(TAG, "BLE reassembler initialized with %" PRIu32 "ms timeout", timeout_ms_);
}

BLEReassembler::~BLEReassembler() {
    stop_timeout();
    if (timeout_timer_) {
        esp_timer_delete(timeout_timer_);
    }
    if (mutex_) {
        vSemaphoreDelete(mutex_);
    }
}

void BLEReassembler::timeout_callback(void* arg) {
    BLEReassembler* self = static_cast<BLEReassembler*>(arg);
    ESP_LOGW(TAG, "Message reassembly timeout after %" PRIu32 "ms", self->timeout_ms_);
    self->reset();
}

uint16_t BLEReassembler::extract_message_length(const uint8_t* data) const {
    if (data == nullptr || LENGTH_PREFIX_SIZE > current_mtu_) {
        return 0;
    }
    // Tesla BLE protocol uses little-endian 16-bit length
    return static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
}

void BLEReassembler::feed(const uint8_t* data, size_t len) {
    if (!data || len == 0 || !callback_) {
        return;
    }
    
    if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to acquire mutex");
        return;
    }
    
    // Start timeout timer if this is the first fragment
    bool is_first_fragment = buffer_.empty();
    if (is_first_fragment) {
        start_timeout();
        
        // Extract total message length from first fragment
        if (len >= LENGTH_PREFIX_SIZE) {
            expected_total_ = extract_message_length(data);
            ESP_LOGD(TAG, "Starting reassembly of %" PRIu16 " byte message", expected_total_);
            
            if (expected_total_ == 0 || expected_total_ > 4096) {  // Sanity check
                ESP_LOGW(TAG, "Invalid message length: %" PRIu16, expected_total_);
                expected_total_ = 0;
                reset();
                xSemaphoreGive(mutex_);
                return;
            }
            
            // Reserve buffer for entire message
            buffer_.reserve(expected_total_);
            
            // Store first fragment (excluding length prefix)
            size_t payload_len = std::min(len - LENGTH_PREFIX_SIZE, 
                                         static_cast<size_t>(expected_total_));
            buffer_.insert(buffer_.end(), 
                          data + LENGTH_PREFIX_SIZE, 
                          data + LENGTH_PREFIX_SIZE + payload_len);
        } else {
            ESP_LOGW(TAG, "First fragment too short: %zu bytes", len);
            reset();
            xSemaphoreGive(mutex_);
            return;
        }
    } else {
        // Subsequent fragment
        stop_timeout();  // Stop and restart timeout
        start_timeout();
        
        // Add fragment to buffer
        size_t remaining = expected_total_ - buffer_.size();
        size_t to_copy = std::min(len, remaining);
        
        buffer_.insert(buffer_.end(), data, data + to_copy);
        
        ESP_LOGD(TAG, "Added fragment: %zu/%" PRIu16 " bytes", 
                buffer_.size(), expected_total_);
    }
    
    // Check if message is complete
    if (!buffer_.empty() && buffer_.size() >= expected_total_) {
        process_complete_message();
        reset();
    }
    
    xSemaphoreGive(mutex_);
}

void BLEReassembler::reset() {
    if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }
    
    stop_timeout();
    buffer_.clear();
    buffer_.shrink_to_fit();
    expected_total_ = 0;
    
    xSemaphoreGive(mutex_);
    ESP_LOGD(TAG, "Reassembler reset");
}

void BLEReassembler::update_mtu(uint16_t mtu) {
    if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }
    
    if (mtu >= 23 && mtu <= 517) {  // BLE MTU valid range
        if (current_mtu_ != mtu) {
            ESP_LOGI(TAG, "MTU updated: %" PRIu16 " -> %" PRIu16, current_mtu_, mtu);
            current_mtu_ = mtu;
        }
    } else {
        ESP_LOGW(TAG, "Invalid MTU: %" PRIu16, mtu);
    }
    
    xSemaphoreGive(mutex_);
}

void BLEReassembler::start_timeout() {
    if (timeout_timer_ && timeout_ms_ > 0) {
        esp_timer_stop(timeout_timer_);
        esp_timer_start_once(timeout_timer_, timeout_ms_ * 1000);
    }
}

void BLEReassembler::stop_timeout() {
    if (timeout_timer_) {
        esp_timer_stop(timeout_timer_);
    }
}

void BLEReassembler::process_complete_message() {
    if (buffer_.empty() || !callback_) {
        return;
    }
    
    ESP_LOGD(TAG, "Message complete: %zu bytes", buffer_.size());
    
    // Call the callback with the complete message
    try {
        callback_(buffer_.data(), buffer_.size());
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "Callback exception: %s", e.what());
    } catch (...) {
        ESP_LOGE(TAG, "Unknown callback exception");
    }
}