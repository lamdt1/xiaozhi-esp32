#include "ir_receiver.h"
#include "settings.h"
#include <esp_log.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <mutex>

#define TAG "IRReceiver"

// Helper function to escape JSON string
static std::string EscapeJsonString(const std::string& str) {
    std::string escaped;
    escaped.reserve(str.length() + 10); // Reserve some extra space for escape sequences
    
    for (char c : str) {
        switch (c) {
            case '"':  escaped += "\\\""; break;
            case '\\': escaped += "\\\\"; break;
            case '\b': escaped += "\\b"; break;
            case '\f': escaped += "\\f"; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default:
                // Escape control characters (0x00-0x1F)
                if (static_cast<unsigned char>(c) < 0x20) {
                    char hex[7];
                    snprintf(hex, sizeof(hex), "\\u%04x", static_cast<unsigned char>(c));
                    escaped += hex;
                } else {
                    escaped += c;
                }
                break;
        }
    }
    
    return escaped;
}

IrReceiver::IrReceiver(gpio_num_t rx_pin)
    : rx_pin_(rx_pin), irrecv_(nullptr), task_handle_(nullptr), running_(false), learning_mode_(false) {
    // Initialize Arduino compatibility layer (required for IRremoteESP8266)
    // Use std::call_once to ensure thread-safe initialization
    static std::once_flag arduino_init_flag;
    std::call_once(arduino_init_flag, []() {
        initArduino();
        ESP_LOGI(TAG, "Arduino compatibility layer initialized");
    });
    
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
    if (running_.load()) {
        ESP_LOGW(TAG, "IR receiver already running");
        return;
    }
    
    // Ensure IR receiver is initialized
    if (irrecv_ == nullptr) {
        ESP_LOGE(TAG, "Cannot start: IR receiver not initialized");
        return;
    }
    
    running_.store(true);
    
    // Create task to process IR signals
    TaskHandle_t new_handle = nullptr;
    BaseType_t result = xTaskCreate(IrTask, "ir_receiver_task", 4096, this, 5, &new_handle);
    if (result != pdPASS || new_handle == nullptr) {
        ESP_LOGE(TAG, "Failed to create IR receiver task (result=%d)", result);
        running_.store(false);
        return;
    }
    
    // Store task handle under mutex protection
    {
        std::lock_guard<std::mutex> lock(task_handle_mutex_);
        task_handle_ = new_handle;
    }
    
    ESP_LOGI(TAG, "IR receiver task started");
}

void IrReceiver::Stop() {
    if (!running_.load()) {
        return;
    }
    
    running_.store(false);
    
    // Wait for task to finish and delete itself
    // The task will delete itself using vTaskDelete(NULL) after ProcessIrTask() returns
    // We just wait for it to complete - do not try to delete it from here
    TaskHandle_t handle_to_wait = nullptr;
    {
        std::lock_guard<std::mutex> lock(task_handle_mutex_);
        handle_to_wait = task_handle_;
    }
    
    if (handle_to_wait != nullptr) {
        // Wait for task to exit its loop and delete itself
        // Give it reasonable time to finish (max 1 second)
        for (int i = 0; i < 10; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
            std::lock_guard<std::mutex> lock(task_handle_mutex_);
            if (task_handle_ == nullptr) {
                break; // Task has deleted itself
            }
        }
        
        // Clear handle if task hasn't deleted itself yet (shouldn't happen normally)
        std::lock_guard<std::mutex> lock(task_handle_mutex_);
        task_handle_ = nullptr;
    }
    
    ESP_LOGI(TAG, "IR receiver task stopped");
}

void IrReceiver::SetCallback(IrCallback callback) {
    callback_ = callback;
}

void IrReceiver::SetLearningMode(bool enabled) {
    learning_mode_.store(enabled);
    ESP_LOGI(TAG, "IR learning mode %s", enabled ? "enabled" : "disabled");
}

void IrReceiver::SetLearningCallback(IrLearningCallback callback) {
    std::lock_guard<std::mutex> lock(learning_callback_mutex_);
    learning_callback_ = callback;
}

void IrReceiver::SaveLearnedCode(const std::string& name, decode_type_t protocol, uint64_t value, uint16_t bits) {
    // Safe logging - validate parameters first
    const char* name_ptr = name.c_str();
    if (name_ptr == nullptr) {
        ESP_LOGE(TAG, "SaveLearnedCode called with null name pointer");
        return;
    }
    
    ESP_LOGI(TAG, "SaveLearnedCode: name_len=%zu protocol=%d value=0x%llx bits=%u", 
             name.length(), protocol, value, bits);
    
    Settings settings("ir_codes", true);
    
    // Validate and truncate name length to prevent NVS key too long error
    // NVS key limit is 15 chars, "code_" prefix is 5 chars, so max name length is 10
    // Note: We count bytes, not UTF-8 characters, to ensure NVS key limit
    const size_t max_name_length = 10;
    std::string truncated_name = name;
    if (name.length() > max_name_length) {
        // Truncate by bytes (not UTF-8 chars) to ensure NVS key limit
        truncated_name = name.substr(0, max_name_length);
        ESP_LOGW(TAG, "IR code name too long (%zu bytes), truncated to %zu bytes", 
                 name.length(), truncated_name.length());
    }
    
    // Create a JSON-like string to store IR code info
    // NVS key limit is 15 chars, so "code_" (5) + name (max 10) = 15 max
    char code_key[16];  // 15 chars + null terminator
    int written = snprintf(code_key, sizeof(code_key), "code_%s", truncated_name.c_str());
    if (written < 0 || static_cast<size_t>(written) >= sizeof(code_key)) {
        ESP_LOGE(TAG, "Failed to create storage key for IR code name: %s (key would be %d chars)", 
                 truncated_name.c_str(), written);
        return;
    }
    
    // Double-check key length (should never exceed 15, but safety check)
    if (strlen(code_key) > 15) {
        ESP_LOGE(TAG, "NVS key too long: %s (%zu chars), truncating name further", code_key, strlen(code_key));
        // Truncate name to ensure key is exactly 15 chars
        size_t max_name_for_key = 10;  // 15 - 5 ("code_")
        if (truncated_name.length() > max_name_for_key) {
            truncated_name = truncated_name.substr(0, max_name_for_key);
            written = snprintf(code_key, sizeof(code_key), "code_%s", truncated_name.c_str());
            if (written < 0 || static_cast<size_t>(written) >= sizeof(code_key)) {
                ESP_LOGE(TAG, "Failed to create storage key even after truncation");
                return;
            }
        }
    }
    
    char code_value[128];
    snprintf(code_value, sizeof(code_value), "{\"protocol\":%d,\"value\":%llu,\"bits\":%d}", 
             protocol, value, bits);
    
    settings.SetString(code_key, code_value);
    
    // Also save the list of learned code names
    // Check for exact token match (not substring match) in comma-delimited list
    std::string code_list = settings.GetString("code_list", "");
    bool name_exists = false;
    
    if (!code_list.empty()) {
        size_t pos = 0;
        while (pos < code_list.length()) {
            size_t comma_pos = code_list.find(',', pos);
            std::string code_name = (comma_pos == std::string::npos) ? 
                code_list.substr(pos) : code_list.substr(pos, comma_pos - pos);
            
            // Exact match check (use truncated name if original was truncated)
            std::string truncated_code_name = code_name;
            if (code_name.length() > max_name_length) {
                truncated_code_name = code_name.substr(0, max_name_length);
            }
            if (truncated_code_name == truncated_name) {
                name_exists = true;
                break;
            }
            
            if (comma_pos == std::string::npos) break;
            pos = comma_pos + 1;
        }
    }
    
    // Only add if name doesn't exist (use truncated name to match storage key)
    if (!name_exists) {
        if (!code_list.empty()) {
            code_list += ",";
        }
        code_list += truncated_name;
        settings.SetString("code_list", code_list);
    }
    
    ESP_LOGI(TAG, "Saved IR code: name_len=%zu protocol=%d value=0x%llx bits=%u", 
             truncated_name.length(), protocol, value, bits);
}

bool IrReceiver::DeleteLearnedCode(const std::string& name) {
    ESP_LOGI(TAG, "DeleteLearnedCode called: name_len=%zu", name.length());
    Settings settings("ir_codes", true);
    
    // Validate and truncate name length (same as SaveLearnedCode)
    // NVS key limit is 15 chars, "code_" prefix is 5 chars, so max name length is 10
    const size_t max_name_length = 10;
    std::string truncated_name = name;
    if (name.length() > max_name_length) {
        truncated_name = name.substr(0, max_name_length);
        ESP_LOGW(TAG, "IR code name too long (%zu bytes), truncating to %zu bytes for deletion", 
                 name.length(), truncated_name.length());
    }
    
    // Create the storage key
    char code_key[16];  // 15 chars + null terminator
    int written = snprintf(code_key, sizeof(code_key), "code_%s", truncated_name.c_str());
    if (written < 0 || static_cast<size_t>(written) >= sizeof(code_key)) {
        ESP_LOGE(TAG, "Failed to create storage key for IR code name: name_len=%zu", truncated_name.length());
        return false;
    }
    
    // Check if the code exists before trying to delete
    std::string code_value = settings.GetString(code_key, "");
    if (code_value.empty()) {
        ESP_LOGW(TAG, "IR code not found: name_len=%zu", truncated_name.length());
        return false;
    }
    
    // Erase the code data
    settings.EraseKey(code_key);
    ESP_LOGI(TAG, "Erased IR code data: key=%s", code_key);
    
    // Remove from code_list
    std::string code_list = settings.GetString("code_list", "");
    if (!code_list.empty()) {
        std::string new_code_list = "";
        size_t pos = 0;
        bool found = false;
        
        while (pos < code_list.length()) {
            size_t comma_pos = code_list.find(',', pos);
            std::string code_name = (comma_pos == std::string::npos) ? 
                code_list.substr(pos) : code_list.substr(pos, comma_pos - pos);
            
            // Truncate code_name if needed for comparison
            std::string truncated_code_name = code_name;
            if (code_name.length() > max_name_length) {
                truncated_code_name = code_name.substr(0, max_name_length);
            }
            
            // Skip if this is the code we're deleting
            if (truncated_code_name == truncated_name) {
                found = true;
                ESP_LOGI(TAG, "Removed '%s' from code_list", code_name.c_str());
            } else if (!code_name.empty()) {
                // Keep this code in the list
                if (!new_code_list.empty()) {
                    new_code_list += ",";
                }
                new_code_list += code_name;
            }
            
            if (comma_pos == std::string::npos) break;
            pos = comma_pos + 1;
        }
        
        // Update code_list
        if (found) {
            if (new_code_list.empty()) {
                // If list is now empty, erase the key
                settings.EraseKey("code_list");
                ESP_LOGI(TAG, "Code list is now empty, erased code_list key");
            } else {
                settings.SetString("code_list", new_code_list);
                ESP_LOGI(TAG, "Updated code_list, remaining codes: %s", new_code_list.c_str());
            }
        }
    }
    
    ESP_LOGI(TAG, "Successfully deleted IR code: name_len=%zu", truncated_name.length());
    return true;
}

void IrReceiver::DeleteAllLearnedCodes() {
    ESP_LOGI(TAG, "DeleteAllLearnedCodes called: deleting all learned IR codes");
    Settings settings("ir_codes", true);
    
    // Erase all keys in the ir_codes namespace
    // This will delete all code_* keys and code_list
    settings.EraseAll();
    
    ESP_LOGI(TAG, "Successfully deleted all learned IR codes");
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
            // Use larger buffer to prevent truncation (255 chars total: "code_" + name)
            char code_key[256];
            int written = snprintf(code_key, sizeof(code_key), "code_%s", code_name.c_str());
            if (written < 0 || static_cast<size_t>(written) >= sizeof(code_key)) {
                ESP_LOGW(TAG, "IR code name too long, skipping: %s", code_name.c_str());
                if (comma_pos == std::string::npos) break;
                pos = comma_pos + 1;
                continue;
            }
            std::string code_value = settings.GetString(code_key, "");
            
            if (!code_value.empty()) {
                if (!first) json += ",";
                // Escape code_name to prevent JSON injection
                std::string escaped_name = EscapeJsonString(code_name);
                json += "{\"name\":\"" + escaped_name + "\",\"data\":" + code_value + "}";
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
    
    // Clear task handle before deleting self to prevent Stop() from accessing invalid handle
    // Use mutex to ensure atomic operation
    {
        std::lock_guard<std::mutex> lock(receiver->task_handle_mutex_);
        receiver->task_handle_ = nullptr;
    }
    
    // Task deletes itself - this is the safe pattern in FreeRTOS
    // vTaskDelete(NULL) deletes the calling task
    vTaskDelete(NULL);
}

void IrReceiver::ProcessIrTask() {
    decode_results results;
    
    // Safety check: ensure irrecv_ is valid
    if (irrecv_ == nullptr) {
        ESP_LOGE(TAG, "IR receiver not initialized, task exiting");
        return;
    }
    
    while (running_.load()) {
        // Safety check before each decode
        if (irrecv_ == nullptr) {
            ESP_LOGE(TAG, "IR receiver became null, task exiting");
            break;
        }
        
        // Check if IR data is available
        if (irrecv_->decode(&results)) {
            bool is_learning = learning_mode_.load();
            ESP_LOGI(TAG, "IR decode result: protocol=%d, value=0x%llx, bits=%d, learning_mode=%d", 
                     results.decode_type, results.value, results.bits, is_learning);
            
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
                if (is_learning) {
                    ESP_LOGI(TAG, "Learning mode active, processing IR code...");
                    // Safely copy callback under mutex protection
                    IrLearningCallback callback_copy;
                    {
                        std::lock_guard<std::mutex> lock(learning_callback_mutex_);
                        callback_copy = learning_callback_;
                    }
                    
                    if (callback_copy) {
                        ESP_LOGI(TAG, "Calling learning callback with protocol=%d, value=0x%llx", 
                                 results.decode_type, results.value);
                        // Generate a default name if needed (max 10 chars for NVS key limit)
                        // Use last 6 hex digits of value to keep name short
                        char default_name[11];  // 10 chars + null terminator
                        snprintf(default_name, sizeof(default_name), "IR_%06llx", results.value & 0xFFFFFF);
                        std::string name_str(default_name);
                        callback_copy(results.decode_type, results.value, results.bits, name_str);
                        ESP_LOGI(TAG, "Learning callback completed");
                    } else {
                        ESP_LOGW(TAG, "Learning callback is null, cannot save IR code");
                    }
                }
                
                // Call normal callback if set (and not in learning mode)
                if (callback_ && !is_learning) {
                    callback_(results.decode_type, results.value, results.bits);
                }
            } else {
                ESP_LOGW(TAG, "IR received: UNKNOWN protocol, bits=%d, value=0x%llx", 
                         results.bits, results.value);
                // In learning mode, we should still try to save UNKNOWN protocols
                if (is_learning) {
                    ESP_LOGI(TAG, "Learning mode: attempting to save UNKNOWN protocol code");
                    IrLearningCallback callback_copy;
                    {
                        std::lock_guard<std::mutex> lock(learning_callback_mutex_);
                        callback_copy = learning_callback_;
                    }
                    
                    if (callback_copy) {
                        // Generate short name (max 10 chars for NVS key limit)
                        // Use last 4 hex digits of value to keep name short
                        char default_name[11];  // 10 chars + null terminator
                        snprintf(default_name, sizeof(default_name), "UNK_%04llx", results.value & 0xFFFF);
                        std::string name_str(default_name);
                        callback_copy(results.decode_type, results.value, results.bits, name_str);
                        ESP_LOGI(TAG, "UNKNOWN protocol code saved via learning callback");
                    }
                }
            }
            
            // Resume receiving (important for next value)
            irrecv_->resume();
        }
        
        // Yield CPU for other FreeRTOS tasks (important!)
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

