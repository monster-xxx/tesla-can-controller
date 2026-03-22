#pragma once

#include <vector>
#include <cstdint>
#include <functional>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/**
 * @brief BLE message reassembler with MTU adaptation and timeout handling
 * 
 * This class handles fragmented BLE messages, automatically adapting to
 * negotiated MTU sizes and providing timeout protection against incomplete
 * transmissions.
 */
class BLEReassembler {
public:
    using MessageCallback = std::function<void(const uint8_t* data, size_t len)>;
    
    /**
     * @brief Construct a new BLEReassembler object
     * @param callback Function to call when a complete message is received
     * @param timeout_ms Timeout in milliseconds for incomplete messages
     */
    explicit BLEReassembler(MessageCallback callback, uint32_t timeout_ms = 2000);
    
    /**
     * @brief Destroy the BLEReassembler object
     */
    ~BLEReassembler();
    
    /**
     * @brief Feed received BLE data into the reassembler
     * @param data Pointer to received data
     * @param len Length of received data in bytes
     */
    void feed(const uint8_t* data, size_t len);
    
    /**
     * @brief Reset the reassembler state (e.g., on disconnection)
     */
    void reset();
    
    /**
     * @brief Get the current MTU size
     * @return Current MTU size in bytes
     */
    uint16_t get_mtu() const { return current_mtu_; }
    
    /**
     * @brief Update the MTU size (call after MTU negotiation)
     * @param mtu New MTU size in bytes
     */
    void update_mtu(uint16_t mtu);
    
    /**
     * @brief Check if a message is currently being reassembled
     * @return true if reassembly is in progress
     */
    bool is_reassembling() const { return !buffer_.empty(); }
    
    /**
     * @brief Get the expected total size of current message
     * @return Expected total size in bytes, 0 if no reassembly in progress
     */
    size_t get_expected_total() const { return expected_total_; }
    
    /**
     * @brief Get the current reassembled size
     * @return Current size of reassembled data in bytes
     */
    size_t get_current_size() const { return buffer_.size(); }

private:
    MessageCallback callback_;
    std::vector<uint8_t> buffer_;
    uint16_t expected_total_ = 0;
    uint16_t current_mtu_ = 23;  // Default BLE MTU
    uint32_t timeout_ms_;
    esp_timer_handle_t timeout_timer_ = nullptr;
    SemaphoreHandle_t mutex_;
    
    static void timeout_callback(void* arg);
    void start_timeout();
    void stop_timeout();
    void process_complete_message();
    uint16_t extract_message_length(const uint8_t* data) const;
    
    // Delete copy constructor and assignment operator
    BLEReassembler(const BLEReassembler&) = delete;
    BLEReassembler& operator=(const BLEReassembler&) = delete;
};