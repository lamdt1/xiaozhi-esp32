#ifndef IR_RECEIVER_H_
#define IR_RECEIVER_H_

#include "Arduino.h"
#include <IRrecv.h>
#include <IRremoteESP8266.h>
#include <IRutils.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/gpio.h>
#include <esp_log.h>
#include <functional>
#include <atomic>

class IrReceiver {
public:
    using IrCallback = std::function<void(decode_type_t protocol, uint64_t value, uint16_t bits)>;
    using IrLearningCallback = std::function<void(decode_type_t protocol, uint64_t value, uint16_t bits, const std::string& name)>;

    IrReceiver(gpio_num_t rx_pin);
    ~IrReceiver();

    void Start();
    void Stop();
    void SetCallback(IrCallback callback);
    
    // IR Learning functions
    void SetLearningMode(bool enabled);
    bool IsLearningMode() const { return learning_mode_.load(); }
    void SetLearningCallback(IrLearningCallback callback);
    void SaveLearnedCode(const std::string& name, decode_type_t protocol, uint64_t value, uint16_t bits);
    std::string GetLearnedCodes() const;

private:
    static void IrTask(void* arg);
    void ProcessIrTask();

    gpio_num_t rx_pin_;
    IRrecv* irrecv_;
    TaskHandle_t task_handle_;
    IrCallback callback_;
    IrLearningCallback learning_callback_;
    std::atomic<bool> running_;
    std::atomic<bool> learning_mode_;
};

#endif // IR_RECEIVER_H_

