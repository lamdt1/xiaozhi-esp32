#ifndef IR_RECEIVER_H_
#define IR_RECEIVER_H_

// Undefine INADDR_NONE macro from lwip to avoid conflict with Arduino's IPAddress.h
// This macro is defined by lwip but Arduino's IPAddress.h tries to use it as a variable name
#ifdef INADDR_NONE
#undef INADDR_NONE
#endif

#include "Arduino.h"
#include <IRrecv.h>
#include <IRremoteESP8266.h>
#include <IRutils.h>
#include <IRsend.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/gpio.h>
#include <esp_log.h>
#include <functional>
#include <atomic>
#include <mutex>
#include <string>

// Maximum number of IR codes that can be stored
// NVS can handle up to 4000 bytes per value, and each code name is max 10 chars
// With comma separators, this allows for many codes (100+ codes = ~1100 bytes)
// Set to a reasonable limit to prevent storage issues
#define MAX_IR_CODES 100

class IrReceiver {
public:
    using IrCallback = std::function<void(decode_type_t protocol, uint64_t value, uint16_t bits)>;
    using IrLearningCallback = std::function<void(decode_type_t protocol, uint64_t value, uint16_t bits, const std::string& name)>;
    using IrRawLearningCallback = std::function<void(const uint16_t* raw_data, uint16_t raw_len, const std::string& name)>;

    IrReceiver(gpio_num_t rx_pin, gpio_num_t tx_pin = GPIO_NUM_NC);
    ~IrReceiver();

    void Start();
    void Stop();
    void SetCallback(IrCallback callback);
    
    // IR Learning functions
    void SetLearningMode(bool enabled);
    bool IsLearningMode() const { return learning_mode_.load(); }
    void SetLearningCallback(IrLearningCallback callback);
    void SetRawLearningCallback(IrRawLearningCallback callback);
    void SaveLearnedCode(const std::string& name, decode_type_t protocol, uint64_t value, uint16_t bits);
    void SaveRawCode(const std::string& name, const uint16_t* raw_data, uint16_t raw_len);
    bool DeleteLearnedCode(const std::string& name);
    void DeleteAllLearnedCodes();
    std::string GetLearnedCodes() const;
    
    // IR Transmitting functions
    bool SendIrCode(decode_type_t protocol, uint64_t value, uint16_t bits);
    bool SendLearnedCode(const std::string& name);
    bool SendRawCode(const uint16_t* raw_data, uint16_t raw_len, uint16_t frequency = 38000);
    bool SendLearnedRawCode(const std::string& name);
    
    // Export learned codes as C++ constants
    std::string ExportAsConstants() const;

private:
    static void IrTask(void* arg);
    void ProcessIrTask();

    gpio_num_t rx_pin_;
    gpio_num_t tx_pin_;
    IRrecv* irrecv_;
    IRsend* irsend_;
    TaskHandle_t task_handle_;
    std::mutex task_handle_mutex_;  // Protects task_handle_ access
    IrCallback callback_;
    IrLearningCallback learning_callback_;
    IrRawLearningCallback raw_learning_callback_;
    std::mutex learning_callback_mutex_;  // Protects learning_callback_ access
    std::atomic<bool> running_;
    std::atomic<bool> learning_mode_;
};

#endif // IR_RECEIVER_H_

