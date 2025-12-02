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

IrReceiver::IrReceiver(gpio_num_t rx_pin, gpio_num_t tx_pin)
    : rx_pin_(rx_pin), tx_pin_(tx_pin), irrecv_(nullptr), irsend_(nullptr), task_handle_(nullptr), running_(false), learning_mode_(false) {
    // Initialize Arduino compatibility layer (required for IRremoteESP8266)
    // Use std::call_once to ensure thread-safe initialization
    static std::once_flag arduino_init_flag;
    std::call_once(arduino_init_flag, []() {
        initArduino();
        ESP_LOGI(TAG, "Arduino compatibility layer initialized");
    });
    
    // Create IR receiver instance with a larger buffer and timeout to handle complex signals (e.g., air conditioners)
    // Convert gpio_num_t to uint16_t for IRremoteESP8266 library
    const uint16_t kCaptureBufferSize = 2048;  // Increase buffer size to 2048 to capture long IR signals
    const uint8_t kCaptureTimeout = 150;       // Increase timeout to 150ms

    irrecv_ = new IRrecv(static_cast<uint16_t>(rx_pin_), kCaptureBufferSize, kCaptureTimeout, true);
    if (irrecv_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create IRrecv instance");
        return;
    }
    
    // Enable IR receiver
    irrecv_->enableIRIn();
    ESP_LOGI(TAG, "IR receiver started on pin %d", rx_pin_);
    
    // Initialize IR transmitter if TX pin is provided
    if (tx_pin_ != GPIO_NUM_NC) {
        irsend_ = new IRsend(static_cast<uint16_t>(tx_pin_));
        if (irsend_ == nullptr) {
            ESP_LOGE(TAG, "Failed to create IRsend instance");
            return;
        }
        irsend_->begin();
        ESP_LOGI(TAG, "IR transmitter started on pin %d", tx_pin_);
    }
}

IrReceiver::~IrReceiver() {
    Stop();
    
    if (irrecv_ != nullptr) {
        irrecv_->disableIRIn();
        delete irrecv_;
        irrecv_ = nullptr;
    }
    
    if (irsend_ != nullptr) {
        delete irsend_;
        irsend_ = nullptr;
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

void IrReceiver::SetRawLearningCallback(IrRawLearningCallback callback) {
    std::lock_guard<std::mutex> lock(learning_callback_mutex_);
    raw_learning_callback_ = callback;
}

void IrReceiver::SaveLearnedCode(const std::string& name, decode_type_t protocol, uint64_t value, uint16_t bits) {
    // Safe logging - validate parameters first
    const char* name_ptr = name.c_str();
    if (name_ptr == nullptr) {
        ESP_LOGE(TAG, "SaveLearnedCode called with null name pointer");
        return;
    }
    
    ESP_LOGI(TAG, "SaveLearnedCode: name_len=%u protocol=%d value=0x%llx bits=%u", 
             (unsigned int)name.length(), protocol, value, bits);
    
    Settings settings("ir_codes", true);
    
    // Validate and truncate name length to prevent NVS key too long error
    // NVS key limit is 15 chars, "code_" prefix is 5 chars, so max name length is 10
    // Note: We count bytes, not UTF-8 characters, to ensure NVS key limit
    const size_t max_name_length = 10;
    std::string truncated_name = name;
    if (name.length() > max_name_length) {
        // Truncate by bytes (not UTF-8 chars) to ensure NVS key limit
        truncated_name = name.substr(0, max_name_length);
        ESP_LOGW(TAG, "IR code name too long (%u bytes), truncated to %u bytes", 
                 (unsigned int)name.length(), (unsigned int)truncated_name.length());
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
        ESP_LOGE(TAG, "NVS key too long: %s (%u chars), truncating name further", code_key, (unsigned int)strlen(code_key));
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
    size_t code_count = 0;
    
    if (!code_list.empty()) {
        size_t pos = 0;
        while (pos < code_list.length()) {
            size_t comma_pos = code_list.find(',', pos);
            std::string code_name = (comma_pos == std::string::npos) ? 
                code_list.substr(pos) : code_list.substr(pos, comma_pos - pos);
            
            if (!code_name.empty()) {
                code_count++;
            }
            
            // Exact match check (use truncated name if original was truncated)
            std::string truncated_code_name = code_name;
            if (code_name.length() > max_name_length) {
                truncated_code_name = code_name.substr(0, max_name_length);
            }
            if (truncated_code_name == truncated_name) {
                name_exists = true;
                // Don't break here - continue counting to get accurate count
            }
            
            if (comma_pos == std::string::npos) break;
            pos = comma_pos + 1;
        }
    }
    
    // Check if we've reached the maximum number of codes
    if (!name_exists && code_count >= MAX_IR_CODES) {
        ESP_LOGW(TAG, "Maximum number of IR codes (%d) reached. Cannot save new code '%s'. Delete some codes first.", 
                 MAX_IR_CODES, truncated_name.c_str());
        return;
    }
    
    // Only add if name doesn't exist (use truncated name to match storage key)
    if (!name_exists) {
        if (!code_list.empty()) {
            code_list += ",";
        }
        code_list += truncated_name;
        settings.SetString("code_list", code_list);
        ESP_LOGI(TAG, "Added '%s' to code_list. New list: %s", truncated_name.c_str(), code_list.c_str());
    } else {
        ESP_LOGI(TAG, "Code '%s' already exists in list, updating data only", truncated_name.c_str());
    }
    
    ESP_LOGI(TAG, "Saved IR code: name='%s' (len=%u) protocol=%d value=0x%llx bits=%u", 
             truncated_name.c_str(), (unsigned int)truncated_name.length(), protocol, value, bits);
}

void IrReceiver::SaveRawCode(const std::string& name, const uint16_t* raw_data, uint16_t raw_len) {
    if (raw_data == nullptr || raw_len == 0) {
        ESP_LOGE(TAG, "SaveRawCode: invalid raw data");
        return;
    }
    
    ESP_LOGI(TAG, "SaveRawCode: name_len=%u raw_len=%u", (unsigned int)name.length(), raw_len);
    
    Settings settings("ir_codes", true);
    
    // Validate and truncate name length
    const size_t max_name_length = 10;
    std::string truncated_name = name;
    if (name.length() > max_name_length) {
        truncated_name = name.substr(0, max_name_length);
        ESP_LOGW(TAG, "IR code name too long (%u bytes), truncated to %u bytes", 
                 (unsigned int)name.length(), (unsigned int)truncated_name.length());
    }
    
    // Create storage key for raw data
    char raw_key[256];
    int written = snprintf(raw_key, sizeof(raw_key), "raw_%s", truncated_name.c_str());
    if (written < 0 || static_cast<size_t>(written) >= sizeof(raw_key)) {
        ESP_LOGE(TAG, "Failed to create storage key for raw IR code");
        return;
    }
    
    // Convert raw data to comma-separated string for storage
    // Format: "len:value1,value2,value3,..."
    std::string raw_str = std::to_string(raw_len) + ":";
    for (uint16_t i = 0; i < raw_len; i++) {
        if (i > 0) raw_str += ",";
        raw_str += std::to_string(raw_data[i]);
    }
    
    // Check NVS value size limit (4000 bytes)
    if (raw_str.length() > 3900) {
        ESP_LOGE(TAG, "Raw data too large (%u bytes), cannot save", (unsigned int)raw_str.length());
        return;
    }
    
    settings.SetString(raw_key, raw_str);
    ESP_LOGI(TAG, "Saved raw IR code: name='%s' raw_len=%u data_size=%u", 
             truncated_name.c_str(), raw_len, (unsigned int)raw_str.length());
    
    // Also add to code_list if not exists
    std::string code_list = settings.GetString("code_list", "");
    bool name_exists = false;
    
    if (!code_list.empty()) {
        size_t pos = 0;
        while (pos < code_list.length()) {
            size_t comma_pos = code_list.find(',', pos);
            std::string code_name = (comma_pos == std::string::npos) ? 
                code_list.substr(pos) : code_list.substr(pos, comma_pos - pos);
            
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
    
    if (!name_exists) {
        if (!code_list.empty()) {
            code_list += ",";
        }
        code_list += truncated_name;
        settings.SetString("code_list", code_list);
        ESP_LOGI(TAG, "Added '%s' to code_list for raw code", truncated_name.c_str());
    }
}

bool IrReceiver::DeleteLearnedCode(const std::string& name) {
    ESP_LOGI(TAG, "DeleteLearnedCode called: name_len=%u", (unsigned int)name.length());
    Settings settings("ir_codes", true);
    
    // Validate and truncate name length (same as SaveLearnedCode)
    // NVS key limit is 15 chars, "code_" prefix is 5 chars, so max name length is 10
    const size_t max_name_length = 10;
    std::string truncated_name = name;
    if (name.length() > max_name_length) {
        truncated_name = name.substr(0, max_name_length);
        ESP_LOGW(TAG, "IR code name too long (%u bytes), truncating to %u bytes for deletion", 
                 (unsigned int)name.length(), (unsigned int)truncated_name.length());
    }
    
    // Create the storage key
    char code_key[16];  // 15 chars + null terminator
    int written = snprintf(code_key, sizeof(code_key), "code_%s", truncated_name.c_str());
    if (written < 0 || static_cast<size_t>(written) >= sizeof(code_key)) {
        ESP_LOGE(TAG, "Failed to create storage key for IR code name: name_len=%u", (unsigned int)truncated_name.length());
        return false;
    }
    
    // Check if the code exists (try both protocol-based and raw data)
    std::string code_value = settings.GetString(code_key, "");
    char raw_key[256];
    int raw_written = snprintf(raw_key, sizeof(raw_key), "raw_%s", truncated_name.c_str());
    std::string raw_value = "";
    if (raw_written >= 0 && static_cast<size_t>(raw_written) < sizeof(raw_key)) {
        raw_value = settings.GetString(raw_key, "");
    }
    
    if (code_value.empty() && raw_value.empty()) {
        ESP_LOGW(TAG, "IR code not found (neither protocol nor raw): name_len=%u", (unsigned int)truncated_name.length());
        return false;
    }
    
    // Erase both protocol-based code and raw data if they exist
    if (!code_value.empty()) {
        settings.EraseKey(code_key);
        ESP_LOGI(TAG, "Erased IR code data: key=%s", code_key);
    }
    if (!raw_value.empty() && raw_written >= 0 && static_cast<size_t>(raw_written) < sizeof(raw_key)) {
        settings.EraseKey(raw_key);
        ESP_LOGI(TAG, "Erased raw IR code data: key=%s", raw_key);
    }
    
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
    
    ESP_LOGI(TAG, "Successfully deleted IR code: name_len=%u", (unsigned int)truncated_name.length());
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
            // Try protocol-based code first (code_*)
            char code_key[256];
            int written = snprintf(code_key, sizeof(code_key), "code_%s", code_name.c_str());
            if (written < 0 || static_cast<size_t>(written) >= sizeof(code_key)) {
                ESP_LOGW(TAG, "IR code name too long, skipping: %s", code_name.c_str());
                if (comma_pos == std::string::npos) break;
                pos = comma_pos + 1;
                continue;
            }
            std::string code_value = settings.GetString(code_key, "");
            
            // If no protocol-based code, try raw data (raw_*)
            if (code_value.empty()) {
                written = snprintf(code_key, sizeof(code_key), "raw_%s", code_name.c_str());
                if (written >= 0 && static_cast<size_t>(written) < sizeof(code_key)) {
                    std::string raw_value = settings.GetString(code_key, "");
                    if (!raw_value.empty()) {
                        // Format raw data as JSON
                        code_value = "{\"type\":\"raw\",\"data\":\"" + EscapeJsonString(raw_value) + "\"}";
                    }
                }
            }
            
            if (!code_value.empty()) {
                if (!first) json += ",";
                // Escape code_name to prevent JSON injection
                std::string escaped_name = EscapeJsonString(code_name);
                // If it's already JSON (protocol-based), use it directly, otherwise wrap it
                if (code_value[0] == '{') {
                    // Already JSON format
                    json += "{\"name\":\"" + escaped_name + "\",\"data\":" + code_value + "}";
                } else {
                    // Raw string, wrap it
                    json += "{\"name\":\"" + escaped_name + "\",\"data\":\"" + EscapeJsonString(code_value) + "\"}";
                }
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
            
            // Validate decode results - filter out invalid/corrupted data
            // Note: Many invalid decodes are just noise/interference, so we log at DEBUG level
            bool is_valid = true;
            if (results.bits > 64) {
                // bits > 64 is usually noise or corrupted signal
                ESP_LOGD(TAG, "Invalid IR decode: bits=%u (out of range), ignoring noise", results.bits);
                is_valid = false;
            }
            if (results.decode_type < 0 || results.decode_type > 100) {
                // protocol=-1 (UNKNOWN) or out of range is common for noise/interference
                ESP_LOGD(TAG, "Invalid IR decode: protocol=%d (out of range), ignoring noise", results.decode_type);
                is_valid = false;
            }
            
            // Even if protocol is invalid, we can still save raw data
            // Check if we have raw data and are in learning mode
            if (!is_valid && is_learning) {
                // Try to save as raw data if raw learning callback is set
                if (results.rawlen > 0 && results.rawbuf != nullptr) {
                    IrRawLearningCallback raw_callback_copy;
                    {
                        std::lock_guard<std::mutex> lock(learning_callback_mutex_);
                        raw_callback_copy = raw_learning_callback_;
                    }
                    
                    if (raw_callback_copy) {
                        ESP_LOGI(TAG, "Invalid protocol but saving as raw data: rawlen=%u", results.rawlen);
                        // Copy volatile rawbuf to non-volatile buffer
                        uint16_t* raw_copy = new (std::nothrow) uint16_t[results.rawlen];
                        if (raw_copy != nullptr) {
                            for (uint16_t i = 0; i < results.rawlen; i++) {
                                raw_copy[i] = results.rawbuf[i];
                            }
                            char default_name[11];
                            snprintf(default_name, sizeof(default_name), "RAW_%04x", 
                                    static_cast<unsigned int>(results.rawlen) & 0xFFFF);
                            std::string name_str(default_name);
                            raw_callback_copy(raw_copy, results.rawlen, name_str);
                            delete[] raw_copy;
                        } else {
                            ESP_LOGE(TAG, "Failed to allocate memory for raw data copy");
                        }
                    }
                }
                irrecv_->resume();
                continue;
            }
            
            if (!is_valid) {
                // Resume and skip invalid data (not in learning mode)
                irrecv_->resume();
                continue;
            }
            
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
                    IrRawLearningCallback raw_callback_copy;
                    {
                        std::lock_guard<std::mutex> lock(learning_callback_mutex_);
                        callback_copy = learning_callback_;
                        raw_callback_copy = raw_learning_callback_;
                    }
                    
                    // Save as protocol-based code if callback is set
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
                    
                    // Also save raw data if raw callback is set and we have raw data
                    if (raw_callback_copy && results.rawlen > 0 && results.rawbuf != nullptr) {
                        ESP_LOGI(TAG, "Also saving raw data: rawlen=%u", results.rawlen);
                        // Copy volatile rawbuf to non-volatile buffer
                        uint16_t* raw_copy = new (std::nothrow) uint16_t[results.rawlen];
                        if (raw_copy != nullptr) {
                            for (uint16_t i = 0; i < results.rawlen; i++) {
                                raw_copy[i] = results.rawbuf[i];
                            }
                            char default_name[11];
                            snprintf(default_name, sizeof(default_name), "RAW_%06llx", results.value & 0xFFFFFF);
                            std::string name_str(default_name);
                            raw_callback_copy(raw_copy, results.rawlen, name_str);
                            delete[] raw_copy;
                        } else {
                            ESP_LOGE(TAG, "Failed to allocate memory for raw data copy");
                        }
                    }
                }
                
                // Call normal callback if set (and not in learning mode)
                if (callback_ && !is_learning) {
                    callback_(results.decode_type, results.value, results.bits);
                }
            } else {
                // UNKNOWN protocol - only log if not in learning mode to reduce log spam
                // In learning mode, we'll log when actually saving
                if (!is_learning) {
                    ESP_LOGD(TAG, "IR received: UNKNOWN protocol, bits=%d, value=0x%llx", 
                             results.bits, results.value);
                } else {
                    // In learning mode, log raw data for debugging (but only if valid)
                    if (results.bits >= 8 && results.bits <= 64 && results.value != 0) {
                        ESP_LOGI(TAG, "UNKNOWN protocol in learning mode: bits=%d, value=0x%llx", 
                                 results.bits, results.value);
                        // Only dump raw data at DEBUG level to avoid log spam
                        ESP_LOGD(TAG, "Raw data: %s", resultToSourceCode(&results).c_str());
                    }
                }
                
                // In learning mode, only save UNKNOWN protocols if they have valid data
                // Filter out invalid/corrupted signals (bits should be reasonable, value should be non-zero)
                if (is_learning) {
                    // Only save if bits is reasonable (between 8 and 64) and value is non-zero
                    // This filters out noise and corrupted signals
                    if (results.bits >= 8 && results.bits <= 64 && results.value != 0) {
                        ESP_LOGI(TAG, "Learning mode: saving UNKNOWN protocol code (bits=%d, value=0x%llx)", 
                                 results.bits, results.value);
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
                    } else {
                        ESP_LOGD(TAG, "Learning mode: ignoring invalid UNKNOWN protocol (bits=%d, value=0x%llx)", 
                                 results.bits, results.value);
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

bool IrReceiver::SendIrCode(decode_type_t protocol, uint64_t value, uint16_t bits) {
    if (irsend_ == nullptr) {
        ESP_LOGE(TAG, "IR transmitter not initialized");
        return false;
    }
    
    ESP_LOGI(TAG, "Sending IR code: protocol=%d, value=0x%llx, bits=%d", protocol, value, bits);
    
    // Send IR code based on protocol type
    // IRremoteESP8266 library handles different protocols
    switch (protocol) {
        case NEC:
            irsend_->sendNEC(value, bits);
            break;
        case SONY:
            irsend_->sendSony(value, bits);
            break;
        case RC5:
            irsend_->sendRC5(value, bits);
            break;
        case RC6:
            irsend_->sendRC6(value, bits);
            break;
        case DISH:
            irsend_->sendDISH(value, bits);
            break;
        case SHARP:
            irsend_->sendSharp(value, bits);
            break;
        case JVC:
            irsend_->sendJVC(value, bits);
            break;
        case SAMSUNG:
            // Samsung protocol typically uses 36 bits
            irsend_->sendSamsung36(value);
            break;
        case LG:
            irsend_->sendLG(value, bits);
            break;
        case WHYNTER:
            irsend_->sendWhynter(value, bits);
            break;
        case COOLIX:
            // Coolix protocol typically uses 48 bits
            irsend_->sendCoolix48(value);
            break;
        case DENON:
            irsend_->sendDenon(value, bits);
            break;
        default:
            // For unknown protocols, try to send raw data if available
            ESP_LOGW(TAG, "Unsupported protocol %d, attempting generic send", protocol);
            // Use sendNEC as fallback for unknown protocols
            irsend_->sendNEC(value, bits);
            break;
    }
    
    return true;
}

bool IrReceiver::SendLearnedCode(const std::string& name) {
    Settings settings("ir_codes");
    
    // Validate and truncate name length (same as SaveLearnedCode)
    const size_t max_name_length = 10;
    std::string truncated_name = name;
    if (name.length() > max_name_length) {
        truncated_name = name.substr(0, max_name_length);
    }
    
    // Create the storage key
    char code_key[256];
    int written = snprintf(code_key, sizeof(code_key), "code_%s", truncated_name.c_str());
    if (written < 0 || static_cast<size_t>(written) >= sizeof(code_key)) {
        ESP_LOGE(TAG, "Failed to create storage key for IR code name");
        return false;
    }
    
    // Get the code data
    std::string code_value = settings.GetString(code_key, "");
    
    // If protocol-based code exists, try to send it
    if (!code_value.empty()) {
        // Parse JSON-like string: {"protocol":X,"value":Y,"bits":Z}
        decode_type_t protocol = UNKNOWN;
        uint64_t value = 0;
        uint16_t bits = 0;
        
        // Simple JSON parsing (assuming format is always the same)
        if (sscanf(code_value.c_str(), "{\"protocol\":%d,\"value\":%llu,\"bits\":%hu}", 
                   reinterpret_cast<int*>(&protocol), &value, &bits) == 3) {
            bool sent = SendIrCode(protocol, value, bits);
            if (sent) {
                return true;
            }
            // If sending failed, fall through to try raw data
            ESP_LOGW(TAG, "Failed to send protocol-based code, trying raw data as fallback");
        } else {
            ESP_LOGW(TAG, "Failed to parse IR code data: %s", code_value.c_str());
        }
    }
    
    // If protocol-based code doesn't exist or failed, try raw data as fallback
    return SendLearnedRawCode(name);
}

bool IrReceiver::SendRawCode(const uint16_t* raw_data, uint16_t raw_len, uint16_t frequency) {
    if (irsend_ == nullptr) {
        ESP_LOGE(TAG, "IR transmitter not initialized");
        return false;
    }
    
    if (raw_data == nullptr || raw_len == 0) {
        ESP_LOGE(TAG, "Invalid raw data: null pointer or zero length");
        return false;
    }
    
    ESP_LOGI(TAG, "Sending raw IR code: raw_len=%u frequency=%u", raw_len, frequency);
    
    // IRsend::sendRaw expects: rawData, length, frequency (kHz)
    irsend_->sendRaw(raw_data, raw_len, frequency);
    
    return true;
}

bool IrReceiver::SendLearnedRawCode(const std::string& name) {
    Settings settings("ir_codes");
    
    // Validate and truncate name length
    const size_t max_name_length = 10;
    std::string truncated_name = name;
    if (name.length() > max_name_length) {
        truncated_name = name.substr(0, max_name_length);
    }
    
    // Create the storage key for raw data
    char raw_key[256];
    int written = snprintf(raw_key, sizeof(raw_key), "raw_%s", truncated_name.c_str());
    if (written < 0 || static_cast<size_t>(written) >= sizeof(raw_key)) {
        ESP_LOGE(TAG, "Failed to create storage key for raw IR code name");
        return false;
    }
    
    // Get the raw data string
    std::string raw_str = settings.GetString(raw_key, "");
    if (raw_str.empty()) {
        ESP_LOGW(TAG, "Raw IR code not found: %s", truncated_name.c_str());
        return false;
    }
    
    // Parse format: "len:value1,value2,value3,..."
    size_t colon_pos = raw_str.find(':');
    if (colon_pos == std::string::npos) {
        ESP_LOGE(TAG, "Invalid raw data format: missing colon");
        return false;
    }
    
    // Parse length with error handling
    uint16_t raw_len = 0;
    try {
        raw_len = static_cast<uint16_t>(std::stoul(raw_str.substr(0, colon_pos)));
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "Failed to parse raw data length: %s", e.what());
        return false;
    }
    
    if (raw_len == 0 || raw_len > 1000) {  // Reasonable limit
        ESP_LOGE(TAG, "Invalid raw data length: %u", raw_len);
        return false;
    }
    
    std::string values_str = raw_str.substr(colon_pos + 1);
    
    // Allocate buffer for raw data
    uint16_t* raw_data = new (std::nothrow) uint16_t[raw_len];
    if (raw_data == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate memory for raw data");
        return false;
    }
    
    // Parse comma-separated values with error handling
    size_t pos = 0;
    uint16_t index = 0;
    bool parse_error = false;
    while (pos < values_str.length() && index < raw_len) {
        size_t comma_pos = values_str.find(',', pos);
        std::string value_str = (comma_pos == std::string::npos) ? 
            values_str.substr(pos) : values_str.substr(pos, comma_pos - pos);
        
        if (!value_str.empty()) {
            try {
                raw_data[index++] = static_cast<uint16_t>(std::stoul(value_str));
            } catch (const std::exception& e) {
                ESP_LOGE(TAG, "Failed to parse raw data value at index %u: %s", index, e.what());
                parse_error = true;
                break;
            }
        }
        
        if (comma_pos == std::string::npos) break;
        pos = comma_pos + 1;
    }
    
    bool result = false;
    if (!parse_error) {
        if (index != raw_len) {
            ESP_LOGW(TAG, "Raw data length mismatch: expected %u, parsed %u", raw_len, index);
        }
        result = SendRawCode(raw_data, index, 38000);  // Default 38kHz
    }
    
    delete[] raw_data;
    return result;
}

std::string IrReceiver::ExportAsConstants() const {
    Settings settings("ir_codes");
    std::string code_list = settings.GetString("code_list", "");
    
    ESP_LOGI(TAG, "ExportAsConstants: code_list='%s'", code_list.c_str());
    
    if (code_list.empty()) {
        ESP_LOGW(TAG, "ExportAsConstants: No codes in code_list");
        return "// No IR codes learned yet\n";
    }
    
    std::string output = "// Auto-generated IR code constants\n";
    output += "// Generated from learned IR codes\n\n";
    output += "#ifndef IR_CODE_CONSTANTS_H_\n";
    output += "#define IR_CODE_CONSTANTS_H_\n\n";
    output += "#include <IRremoteESP8266.h>\n\n";
    output += "namespace IrCodes {\n\n";
    
    size_t pos = 0;
    int exported_count = 0;
    while (pos < code_list.length()) {
        size_t comma_pos = code_list.find(',', pos);
        std::string code_name = (comma_pos == std::string::npos) ? 
            code_list.substr(pos) : code_list.substr(pos, comma_pos - pos);
        
        if (!code_name.empty()) {
            // Convert code name to valid C++ identifier (uppercase, replace invalid chars)
            std::string const_name = code_name;
            for (char& c : const_name) {
                if (c >= 'a' && c <= 'z') {
                    c = c - 'a' + 'A';  // Convert to uppercase
                } else if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) {
                    c = '_';  // Replace invalid chars with underscore
                }
            }
            
            // Truncate code name to match storage key format (max 10 chars)
            // This is important because SaveRawCode and SaveLearnedCode truncate names
            const size_t max_name_length = 10;
            std::string truncated_code_name = code_name;
            if (code_name.length() > max_name_length) {
                truncated_code_name = code_name.substr(0, max_name_length);
            }
            
            bool code_exported = false;
            
            // Try protocol-based code first (code_*)
            char code_key[256];
            int written = snprintf(code_key, sizeof(code_key), "code_%s", truncated_code_name.c_str());
            if (written >= 0 && static_cast<size_t>(written) < sizeof(code_key)) {
                std::string code_value = settings.GetString(code_key, "");
                ESP_LOGI(TAG, "ExportAsConstants: code_name='%s', code_key='%s', code_value='%s'", 
                         code_name.c_str(), code_key, code_value.c_str());
                
                if (!code_value.empty()) {
                    decode_type_t protocol = UNKNOWN;
                    uint64_t value = 0;
                    uint16_t bits = 0;
                    
                    int parsed = sscanf(code_value.c_str(), "{\"protocol\":%d,\"value\":%llu,\"bits\":%hu}", 
                                       reinterpret_cast<int*>(&protocol), &value, &bits);
                    ESP_LOGI(TAG, "ExportAsConstants: parsed=%d, protocol=%d, value=0x%llx, bits=%u", 
                             parsed, protocol, value, bits);
                    
                    if (parsed == 3) {
                        output += "    // " + code_name + " (protocol-based)\n";
                        output += "    constexpr decode_type_t " + const_name + "_PROTOCOL = " + 
                                  std::to_string(protocol) + ";\n";
                        
                        // Convert value to hex string
                        char value_hex[32];
                        snprintf(value_hex, sizeof(value_hex), "%llx", value);
                        output += "    constexpr uint64_t " + const_name + "_VALUE = 0x" + 
                                  std::string(value_hex) + "ULL;\n";
                        output += "    constexpr uint16_t " + const_name + "_BITS = " + 
                                  std::to_string(bits) + ";\n\n";
                        exported_count++;
                        code_exported = true;
                    } else {
                        ESP_LOGW(TAG, "ExportAsConstants: Failed to parse code_value for '%s': '%s'", 
                                 code_name.c_str(), code_value.c_str());
                    }
                }
            }
            
            // If no protocol-based code was exported, try raw data (raw_*)
            if (!code_exported) {
                written = snprintf(code_key, sizeof(code_key), "raw_%s", truncated_code_name.c_str());
                if (written >= 0 && static_cast<size_t>(written) < sizeof(code_key)) {
                    std::string raw_value = settings.GetString(code_key, "");
                    ESP_LOGI(TAG, "ExportAsConstants: code_name='%s', raw_key='%s', raw_value_len=%u", 
                             code_name.c_str(), code_key, (unsigned int)raw_value.length());
                    
                    if (!raw_value.empty()) {
                        // Parse raw data format: "len:value1,value2,value3,..."
                        size_t colon_pos = raw_value.find(':');
                        if (colon_pos != std::string::npos) {
                            uint16_t raw_len = 0;
                            try {
                                raw_len = static_cast<uint16_t>(std::stoul(raw_value.substr(0, colon_pos)));
                            } catch (const std::exception& e) {
                                ESP_LOGW(TAG, "ExportAsConstants: Failed to parse raw_len for '%s': %s", 
                                         code_name.c_str(), e.what());
                                // Skip this raw code entry, but continue processing other codes
                            }
                            
                            if (raw_len > 0) {
                                std::string values_str = raw_value.substr(colon_pos + 1);
                                
                                output += "    // " + code_name + " (raw data)\n";
                                output += "    constexpr uint16_t " + const_name + "_RAW_LEN = " + 
                                          std::to_string(raw_len) + ";\n";
                                output += "    constexpr uint16_t " + const_name + "_RAW_DATA[" + 
                                          std::to_string(raw_len) + "] = {";
                                
                                // Parse comma-separated values
                                size_t pos = 0;
                                uint16_t index = 0;
                                bool first_value = true;
                                while (pos < values_str.length() && index < raw_len) {
                                    size_t comma_pos = values_str.find(',', pos);
                                    std::string value_str = (comma_pos == std::string::npos) ? 
                                        values_str.substr(pos) : values_str.substr(pos, comma_pos - pos);
                                    
                                    if (!value_str.empty()) {
                                        if (!first_value) output += ",";
                                        // Format array with line breaks every 8 values for readability
                                        if (index > 0 && index % 8 == 0) {
                                            output += "\n        ";
                                        }
                                        output += value_str;
                                        first_value = false;
                                        index++;
                                    }
                                    
                                    if (comma_pos == std::string::npos) break;
                                    pos = comma_pos + 1;
                                }
                                
                                output += "};\n";
                                output += "    constexpr uint16_t " + const_name + "_RAW_FREQUENCY = 38000;  // 38kHz\n\n";
                                exported_count++;
                                ESP_LOGI(TAG, "ExportAsConstants: Exported raw data for '%s' (len=%u)", 
                                         code_name.c_str(), index);
                            }
                        } else {
                            ESP_LOGW(TAG, "ExportAsConstants: Invalid raw data format for '%s'", code_name.c_str());
                        }
                    }
                }
            }
        }
        
        if (comma_pos == std::string::npos) break;
        pos = comma_pos + 1;
    }
    
    output += "} // namespace IrCodes\n\n";
    output += "#endif // IR_CODE_CONSTANTS_H_\n";
    
    ESP_LOGI(TAG, "ExportAsConstants: Exported %d codes", exported_count);
    
    if (exported_count == 0) {
        output = "// No valid IR codes found to export\n";
    }
    
    return output;
}

