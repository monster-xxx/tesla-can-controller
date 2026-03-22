#pragma once

#include <vector>
#include <cstdint>
#include <functional>
#include <queue>
#include <memory>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

/**
 * @brief Message priority levels for real-time scheduling
 */
enum class MessagePriority {
    PRIO_CRITICAL = 0,    ///< Safety controls (unlock, honk, lights)
    PRIO_HIGH = 1,        ///< Vehicle status requests
    PRIO_NORMAL = 2,      ///< Comfort controls (climate, seats)
    PRIO_LOW = 3          ///< Media controls
};

/**
 * @brief Message to be scheduled
 */
struct ScheduledMessage {
    MessagePriority priority;
    std::vector<uint8_t> data;
    uint64_t enqueue_time_us;
    uint32_t timeout_ms;
    
    // Comparison for priority queue (lower value = higher priority)
    bool operator<(const ScheduledMessage& other) const {
        if (priority != other.priority) {
            return static_cast<int>(priority) > static_cast<int>(other.priority);
        }
        return enqueue_time_us > other.enqueue_time_us;  // Older messages first
    }
};

/**
 * @brief Real-time message scheduler with priority-based queuing
 * 
 * This class manages message transmission with four priority levels,
 * ensuring critical vehicle commands are sent before less important ones.
 * Uses FreeRTOS queues for thread-safe operation.
 */
class MessageScheduler {
public:
    using SendCallback = std::function<bool(const uint8_t* data, size_t len)>;
    
    /**
     * @brief Construct a new Message Scheduler object
     * @param callback Function to call when a message should be sent
     * @param max_queue_size Maximum number of messages in queue
     */
    explicit MessageScheduler(SendCallback callback, size_t max_queue_size = 100);
    
    /**
     * @brief Destroy the Message Scheduler object
     */
    ~MessageScheduler();
    
    /**
     * @brief Enqueue a message for transmission
     * @param priority Message priority level
     * @param data Message data
     * @param len Message length in bytes
     * @param timeout_ms Timeout in milliseconds (0 = no timeout)
     * @return true if message was enqueued successfully
     */
    bool enqueue(MessagePriority priority, const uint8_t* data, size_t len, uint32_t timeout_ms = 5000);
    
    /**
     * @brief Enqueue a message with vector data
     * @param priority Message priority level
     * @param data Message data as vector
     * @param timeout_ms Timeout in milliseconds (0 = no timeout)
     * @return true if message was enqueued successfully
     */
    bool enqueue(MessagePriority priority, const std::vector<uint8_t>& data, uint32_t timeout_ms = 5000);
    
    /**
     * @brief Start the scheduler task
     * @param stack_size Task stack size in bytes
     * @param priority Task priority (FreeRTOS priority)
     * @param core_id CPU core to pin task to (-1 for no affinity)
     * @return true if task started successfully
     */
    bool start(size_t stack_size = 4096, UBaseType_t priority = 5, int core_id = -1);
    
    /**
     * @brief Stop the scheduler task
     */
    void stop();
    
    /**
     * @brief Clear all pending messages
     */
    void clear();
    
    /**
     * @brief Get the number of pending messages
     * @return Number of messages in queue
     */
    size_t pending_count() const;
    
    /**
     * @brief Get the number of messages sent
     * @return Total messages sent
     */
    size_t sent_count() const { return sent_count_; }
    
    /**
     * @brief Get the number of messages dropped (timeout or queue full)
     * @return Total messages dropped
     */
    size_t dropped_count() const { return dropped_count_; }
    
    /**
     * @brief Set the minimum send interval
     * @param interval_ms Minimum interval between sends in milliseconds
     */
    void set_min_interval(uint32_t interval_ms) { min_interval_ms_ = interval_ms; }
    
    /**
     * @brief Check if scheduler task is running
     * @return true if task is running
     */
    bool is_running() const { return task_handle_ != nullptr; }

private:
    SendCallback callback_;
    std::priority_queue<ScheduledMessage> queue_;
    mutable SemaphoreHandle_t mutex_;
    TaskHandle_t task_handle_ = nullptr;
    size_t max_queue_size_;
    size_t sent_count_ = 0;
    size_t dropped_count_ = 0;
    uint32_t min_interval_ms_ = 10;  // 10ms minimum between sends
    volatile bool running_ = false;
    
    static void scheduler_task(void* arg);
    void process_queue();
    void cleanup_expired_messages();
    bool try_send_message(const ScheduledMessage& msg);
    
    // Delete copy constructor and assignment operator
    MessageScheduler(const MessageScheduler&) = delete;
    MessageScheduler& operator=(const MessageScheduler&) = delete;
};