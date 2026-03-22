#include "message_scheduler.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <algorithm>

static const char* TAG = "msg_scheduler";

MessageScheduler::MessageScheduler(SendCallback callback, size_t max_queue_size)
    : callback_(std::move(callback)), max_queue_size_(max_queue_size) {
    
    mutex_ = xSemaphoreCreateMutex();
    if (!mutex_) {
        ESP_LOGE(TAG, "Failed to create mutex");
    }
    
    ESP_LOGI(TAG, "Message scheduler initialized with max queue size: %zu", max_queue_size_);
}

MessageScheduler::~MessageScheduler() {
    stop();
    if (mutex_) {
        vSemaphoreDelete(mutex_);
    }
}

bool MessageScheduler::enqueue(MessagePriority priority, const uint8_t* data, size_t len, uint32_t timeout_ms) {
    if (!data || len == 0 || !callback_) {
        return false;
    }
    
    if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to acquire mutex for enqueue");
        dropped_count_++;
        return false;
    }
    
    // Check queue size limit
    if (queue_.size() >= max_queue_size_) {
        ESP_LOGW(TAG, "Queue full (%zu/%zu), dropping message", queue_.size(), max_queue_size_);
        dropped_count_++;
        xSemaphoreGive(mutex_);
        return false;
    }
    
    ScheduledMessage msg;
    msg.priority = priority;
    msg.data.assign(data, data + len);
    msg.enqueue_time_us = esp_timer_get_time();
    msg.timeout_ms = timeout_ms;
    
    queue_.push(std::move(msg));
    
    ESP_LOGD(TAG, "Enqueued message: prio=%d, size=%zu, timeout=%" PRIu32 "ms, queue=%zu",
            static_cast<int>(priority), len, timeout_ms, queue_.size());
    
    xSemaphoreGive(mutex_);
    return true;
}

bool MessageScheduler::enqueue(MessagePriority priority, const std::vector<uint8_t>& data, uint32_t timeout_ms) {
    return enqueue(priority, data.data(), data.size(), timeout_ms);
}

bool MessageScheduler::start(size_t stack_size, UBaseType_t priority, int core_id) {
    if (running_ || task_handle_ != nullptr) {
        ESP_LOGW(TAG, "Scheduler already running");
        return false;
    }
    
    running_ = true;
    
    BaseType_t result = xTaskCreatePinnedToCore(
        &MessageScheduler::scheduler_task,
        "msg_scheduler",
        stack_size,
        this,
        priority,
        &task_handle_,
        (core_id >= 0 && core_id < portNUM_PROCESSORS) ? core_id : tskNO_AFFINITY
    );
    
    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create scheduler task");
        running_ = false;
        task_handle_ = nullptr;
        return false;
    }
    
    ESP_LOGI(TAG, "Scheduler task started (stack=%zu, prio=%u, core=%d)",
            stack_size, priority, core_id);
    return true;
}

void MessageScheduler::stop() {
    running_ = false;
    
    if (task_handle_ != nullptr) {
        // Wait for task to finish (max 1 second)
        uint32_t timeout_ms = 1000;
        while (eTaskGetState(task_handle_) != eDeleted && timeout_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
            timeout_ms -= 10;
        }
        
        if (eTaskGetState(task_handle_) != eDeleted) {
            vTaskDelete(task_handle_);
        }
        
        task_handle_ = nullptr;
    }
    
    clear();
    ESP_LOGI(TAG, "Scheduler stopped");
}

void MessageScheduler::clear() {
    if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }
    
    // Clear the queue by swapping with empty queue
    std::priority_queue<ScheduledMessage> empty;
    queue_.swap(empty);
    
    xSemaphoreGive(mutex_);
    ESP_LOGI(TAG, "Message queue cleared");
}

size_t MessageScheduler::pending_count() const {
    if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(50)) != pdTRUE) {
        return 0;
    }
    
    size_t count = queue_.size();
    xSemaphoreGive(mutex_);
    
    return count;
}

void MessageScheduler::scheduler_task(void* arg) {
    MessageScheduler* self = static_cast<MessageScheduler*>(arg);
    
    ESP_LOGI(TAG, "Scheduler task started");
    
    uint32_t last_send_time = 0;
    
    while (self->running_) {
        // Clean up expired messages
        self->cleanup_expired_messages();
        
        // Process next message if minimum interval has passed
        uint32_t now = pdTICKS_TO_MS(xTaskGetTickCount());
        if (now - last_send_time >= self->min_interval_ms_) {
            self->process_queue();
            last_send_time = now;
        }
        
        // Sleep to prevent busy-waiting
        vTaskDelay(pdMS_TO_TICKS(self->min_interval_ms_ / 2));
    }
    
    ESP_LOGI(TAG, "Scheduler task exiting");
    vTaskDelete(nullptr);
}

void MessageScheduler::process_queue() {
    if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }
    
    if (queue_.empty()) {
        xSemaphoreGive(mutex_);
        return;
    }
    
    // Get highest priority message
    ScheduledMessage msg = queue_.top();
    queue_.pop();
    
    xSemaphoreGive(mutex_);
    
    // Try to send the message
    if (try_send_message(msg)) {
        sent_count_++;
        ESP_LOGD(TAG, "Message sent successfully, sent_count=%zu", sent_count_);
    } else {
        // Send failed, could retry or drop
        ESP_LOGW(TAG, "Message send failed, dropping");
        dropped_count_++;
    }
}

void MessageScheduler::cleanup_expired_messages() {
    if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }
    
    if (queue_.empty()) {
        xSemaphoreGive(mutex_);
        return;
    }
    
    uint64_t now_us = esp_timer_get_time();
    size_t initial_size = queue_.size();
    
    // Temporary vector to hold non-expired messages
    std::vector<ScheduledMessage> valid_messages;
    valid_messages.reserve(queue_.size());
    
    while (!queue_.empty()) {
        ScheduledMessage msg = queue_.top();
        queue_.pop();
        
        // Check if message has expired
        if (msg.timeout_ms > 0) {
            uint64_t elapsed_us = now_us - msg.enqueue_time_us;
            uint64_t elapsed_ms = elapsed_us / 1000;
            
            if (elapsed_ms >= msg.timeout_ms) {
                ESP_LOGD(TAG, "Dropping expired message (age=%" PRIu64 "ms, timeout=%" PRIu32 "ms)",
                        elapsed_ms, msg.timeout_ms);
                dropped_count_++;
                continue;
            }
        }
        
        valid_messages.push_back(std::move(msg));
    }
    
    // Rebuild the queue with valid messages
    for (auto& msg : valid_messages) {
        queue_.push(std::move(msg));
    }
    
    if (initial_size != queue_.size()) {
        ESP_LOGD(TAG, "Cleaned up %zu expired messages", initial_size - queue_.size());
    }
    
    xSemaphoreGive(mutex_);
}

bool MessageScheduler::try_send_message(const ScheduledMessage& msg) {
    if (!callback_) {
        return false;
    }
    
    try {
        return callback_(msg.data.data(), msg.data.size());
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "Send callback exception: %s", e.what());
        return false;
    } catch (...) {
        ESP_LOGE(TAG, "Unknown send callback exception");
        return false;
    }
}