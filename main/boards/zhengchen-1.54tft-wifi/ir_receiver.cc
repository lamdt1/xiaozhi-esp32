#include "ir_receiver.h"
#include "settings.h"
#include <esp_log.h>
#include <cstdio>
#include <cstring>

#define TAG "IRReceiver"

IrReceiver::IrReceiver(gpio_num_t rx_pin)
    : rx_pin_(rx_pin), irrecv_(nullptr), task_handle_(nullptr), running_(false), learning_mode_(false) {
    // Initialize Arduino compatibility layer (required for IRremoteESP8266)
    // Only initialize once - check if already initialized to avoid issues
    static bool arduino_initialized = false;
    if (!arduino_initialized) {
        initArduino();
        arduino_initialized = true;
        ESP_LOGI(TAG, "Arduino compatibility layer initialized");
    }
    
    // Create IR receiver instance with default buffer size and timeout
    // Convert gpio_num_t to uint16_t for IRremoteESP8266 library
    irrecv_ = new IRrecv(static_cast<uint16_t>(rx_pin_), kRawBuf, kTimeoutMs);
    if (irrecv_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create IRrecv instance");
        return;
    }
    
    // Enable IR receiver
    irrecv_->enableIRIn();
    ESP_LOGI(TAG, "IR receiver started on pin %d", rx_pin_);
}

IrReceiver::~IrReceiver() {
    Stop();
    
    if (irrecv_ != nullptr) {
        irrecv_->disableIRIn();
        delete irrecv_;
        irrecv_ = nullptr;
    }
    
    ESP_LOGI(TAG, "IR receiver destroyed");
}

void IrReceiver::Start() {
    if (running_) {
        ESP_LOGW(TAG, "IR receiver already running");
        return;
    }
    
    // Ensure IR receiver is initialized
    if (irrecv_ == nullptr) {
        ESP_LOGE(TAG, "Cannot start: IR receiver not initialized");
        return;
    }
    
    running_ = true;
    
    // Create task to process IR signals
    BaseType_t result = xTaskCreate(IrTask, "ir_receiver_task", 4096, this, 5, &task_handle_);
    if (result != pdPASS || task_handle_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create IR receiver task (result=%d)", result);
        running_ = false;
        return;
    }
    
    ESP_LOGI(TAG, "IR receiver task started");
}

void IrReceiver::Stop() {
    if (!running_) {
        return;
    }
    
    running_ = false;
    
    // Wait for task to finish (with timeout)
    if (task_handle_ != nullptr) {
        // Give task time to exit
        vTaskDelay(pdMS_TO_TICKS(100));
        
        // Delete task if still running
        if (eTaskGetState(task_handle_) != eDeleted) {
            vTaskDelete(task_handle_);
        }
        task_handle_ = nullptr;
    }
    
    ESP_LOGI(TAG, "IR receiver task stopped");
}

void IrReceiver::SetCallback(IrCallback callback) {
    callback_ = callback;
}

void IrReceiver::SetLearningMode(bool enabled) {
    learning_mode_ = enabled;
    ESP_LOGI(TAG, "IR learning mode %s", enabled ? "enabled" : "disabled");
}

void IrReceiver::SetLearningCallback(IrLearningCallback callback) {
    learning_callback_ = callback;
}

void IrReceiver::SaveLearnedCode(const std::string& name, decode_type_t protocol, uint64_t value, uint16_t bits) {
    Settings settings("ir_codes", true);
    
    // Create a JSON-like string to store IR code info
    char code_key[64];
    snprintf(code_key, sizeof(code_key), "code_%s", name.c_str());
    
    char code_value[128];
    snprintf(code_value, sizeof(code_value), "{\"protocol\":%d,\"value\":%llu,\"bits\":%d}", 
             protocol, value, bits);
    
    settings.SetString(code_key, code_value);
    
    // Also save the list of learned code names
    std::string code_list = settings.GetString("code_list", "");
    if (code_list.find(name) == std::string::npos) {
        if (!code_list.empty()) {
            code_list += ",";
        }
        code_list += name;
        settings.SetString("code_list", code_list);
    }
    
    ESP_LOGI(TAG, "Saved IR code: %s (protocol=%d, value=0x%llx)", name.c_str(), protocol, value);
}

std::string IrReceiver::GetLearnedCodes() const {
    Settings settings("ir_codes");
    std::string code_list = settings.GetString("code_list", "");
    
    if (code_list.empty()) {
        return "{\"codes\":[]}";
    }
    
    // Parse code_list and build JSON response
    std::string json = "{\"codes\":[";
    size_t pos = 0;
    bool first = true;
    
    while (pos < code_list.length()) {
        size_t comma_pos = code_list.find(',', pos);
        std::string code_name = (comma_pos == std::string::npos) ? 
            code_list.substr(pos) : code_list.substr(pos, comma_pos - pos);
        
        if (!code_name.empty()) {
            char code_key[64];
            snprintf(code_key, sizeof(code_key), "code_%s", code_name.c_str());
            std::string code_value = settings.GetString(code_key, "");
            
            if (!code_value.empty()) {
                if (!first) json += ",";
                json += "{\"name\":\"" + code_name + "\",\"data\":" + code_value + "}";
                first = false;
            }
        }
        
        if (comma_pos == std::string::npos) break;
        pos = comma_pos + 1;
    }
    
    json += "]}";
    return json;
}

void IrReceiver::IrTask(void* arg) {
    IrReceiver* receiver = static_cast<IrReceiver*>(arg);
    if (receiver == nullptr) {
        ESP_LOGE(TAG, "Invalid receiver pointer in task");
        vTaskDelete(NULL);
        return;
    }
    
    receiver->ProcessIrTask();
    vTaskDelete(NULL);
}

void IrReceiver::ProcessIrTask() {
    decode_results results;
    
    // Safety check: ensure irrecv_ is valid
    if (irrecv_ == nullptr) {
        ESP_LOGE(TAG, "IR receiver not initialized, task exiting");
        return;
    }
    
    while (running_) {
        // Safety check before each decode
        if (irrecv_ == nullptr) {
            ESP_LOGE(TAG, "IR receiver became null, task exiting");
            break;
        }
        
        // Check if IR data is available
        if (irrecv_->decode(&results)) {
            // Process the decoded result
            if (results.decode_type != UNKNOWN) {
                // Print IR value in HEX format (following the guide)
                ESP_LOGI(TAG, "IR received: protocol=%d, value=0x%llx, bits=%d", 
                         results.decode_type, results.value, results.bits);
                
                // Try to print using serialPrintUint64, but don't crash if it fails
                // (Serial might not be initialized in ESP-IDF)
                #ifdef ARDUINO
                serialPrintUint64(results.value, HEX);
                printf("\n");
                #endif
                
                // If in learning mode, trigger learning callback
                if (learning_mode_ && learning_callback_) {
                    // Generate a default name if needed
                    char default_name[32];
                    snprintf(default_name, sizeof(default_name), "IR_%llx", results.value);
                    learning_callback_(results.decode_type, results.value, results.bits, default_name);
                }
                
                // Call normal callback if set (and not in learning mode)
                if (callback_ && !learning_mode_) {
                    callback_(results.decode_type, results.value, results.bits);
                }
            } else {
                ESP_LOGD(TAG, "IR received: UNKNOWN protocol, bits=%d", results.bits);
            }
            
            // Resume receiving (important for next value)
            irrecv_->resume();
        }
        
        // Yield CPU for other FreeRTOS tasks (important!)
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

