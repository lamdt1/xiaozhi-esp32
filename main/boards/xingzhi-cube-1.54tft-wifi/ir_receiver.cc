#include "ir_receiver.h"
#include <esp_log.h>
#include <cinttypes>
#include <algorithm>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sys/queue.h>

#define TAG "IRReceiver"

// Protocol timing constants (in microseconds)
#define NEC_HEADER_MARK 9000
#define NEC_HEADER_SPACE 4500
#define NEC_BIT_MARK 560
#define NEC_ONE_SPACE 1690
#define NEC_ZERO_SPACE 560
#define NEC_REPEAT_SPACE 2250
#define NEC_TOLERANCE 200

#define RC5_BIT_DURATION 1778
#define RC5_TOLERANCE 300

#define SONY_HEADER_MARK 2400
#define SONY_HEADER_SPACE 600
#define SONY_BIT_MARK 600
#define SONY_ONE_SPACE 1200
#define SONY_ZERO_SPACE 600
#define SONY_TOLERANCE 200

IRReceiver::IRReceiver(gpio_num_t gpio_num) 
    : gpio_num_(gpio_num), 
      ir_learn_handle_(nullptr),
      is_running_(false),
      last_command_(0),
      last_protocol_("Unknown") {
}

IRReceiver::~IRReceiver() {
    Stop();
    
    // Delete ir_learn handle in destructor to free resources
    if (ir_learn_handle_ != nullptr) {
        ir_learn_del(&ir_learn_handle_);
        ir_learn_handle_ = nullptr;
    }
}

void IRReceiver::Start() {
    if (is_running_) {
        ESP_LOGW(TAG, "IR Receiver already running");
        return;
    }

    // Create ir_learn handle if not exists
    if (ir_learn_handle_ == nullptr) {
        ir_learn_cfg_t config = {
            .learn_count = 1,  // Learn once per command
            .learn_gpio = gpio_num_,
            .clk_src = RMT_CLK_SRC_DEFAULT,
            .resolution = 1000000,  // 1MHz = 1us per tick
            .task_stack = 4096,
            .task_priority = 5,
            .task_affinity = -1,  // No affinity
            .callback = ir_learn_callback,
            .user_data = this,  // Pass this instance as user data
        };
        
        esp_err_t ret = ir_learn_new(&config, &ir_learn_handle_);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create ir_learn handle: %s (0x%x)", esp_err_to_name(ret), ret);
            ir_learn_handle_ = nullptr;
            return;
        }
        
        // Note: ir_learn callback receives user_data from config, not via separate setter
    }
    
    // Start learning
    esp_err_t ret = ir_learn_restart(ir_learn_handle_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start ir_learn: %s", esp_err_to_name(ret));
        return;
    }
    
    is_running_ = true;
    ESP_LOGI(TAG, "IR Receiver started on GPIO %d", gpio_num_);
}

void IRReceiver::Stop() {
    if (!is_running_) {
        return;
    }

    // Mark as stopped first
    is_running_ = false;

    if (ir_learn_handle_ != nullptr) {
        // Stop learning (pass pointer to handle)
        ir_learn_stop(&ir_learn_handle_);
    }
    
    ESP_LOGI(TAG, "IR Receiver stopped");
}

void IRReceiver::OnCommandReceived(CommandCallback callback) {
    command_callback_ = callback;
}

void IRReceiver::OnRawDataReceived(RawDataCallback callback) {
    raw_data_callback_ = callback;
}

void IRReceiver::Process() {
    // Processing is done in the callback
}

void IRReceiver::ir_learn_callback(ir_learn_state_t state, uint8_t sub_step, 
                                    struct ir_learn_sub_list_head *data, void *user_data) {
    IRReceiver* receiver = static_cast<IRReceiver*>(user_data);
    if (receiver == nullptr) {
        return;
    }
    
    receiver->ProcessLearnedData(state, sub_step, data);
}

void IRReceiver::ProcessLearnedData(ir_learn_state_t state, uint8_t sub_step, struct ir_learn_sub_list_head *data) {
    switch (state) {
        case IR_LEARN_STATE_READY:
            ESP_LOGI(TAG, "IR Learn ready");
            break;
            
        case IR_LEARN_STATE_END:
            ESP_LOGI(TAG, "IR Learn end");
            if (data != nullptr) {
                // Convert ir_learn data to raw_data vector
                raw_data_.clear();
                
                // Iterate through the linked list of IR timing data
                // ir_learn uses SLIST (singly-linked list) structure
                struct ir_learn_sub_list_t *entry;
                SLIST_FOREACH(entry, data, next) {
                    // ir_learn provides timing data in microseconds
                    // Each entry contains mark and space durations
                    raw_data_.push_back(entry->mark);
                    raw_data_.push_back(entry->space);
                }
                
                // Try to decode known protocols
                uint64_t command = 0;
                std::string protocol = "Raw";
                
                if (DecodeNEC(raw_data_, command)) {
                    protocol = "NEC";
                } else if (DecodeRC5(raw_data_, command)) {
                    protocol = "RC5";
                } else if (DecodeSony(raw_data_, command)) {
                    protocol = "Sony";
                } else {
                    // Raw data, use first few timings as "command" for compatibility
                    if (raw_data_.size() >= 2) {
                        command = (static_cast<uint64_t>(raw_data_[0]) << 32) | raw_data_[1];
                    }
                }
                
                last_command_ = command;
                last_protocol_ = protocol;
                
                // Call callbacks if set
                if (command_callback_ && is_running_) {
                    command_callback_(command, protocol);
                }
                
                if (raw_data_callback_ && is_running_) {
                    raw_data_callback_(raw_data_);
                }
            }
            break;
            
        case IR_LEARN_STATE_FAIL:
            ESP_LOGW(TAG, "IR Learn failed, retry");
            break;
            
        case IR_LEARN_STATE_EXIT:
            ESP_LOGI(TAG, "IR Learn exit");
            break;
            
        case IR_LEARN_STATE_STEP:
        default:
            ESP_LOGD(TAG, "IR Learn step:[%d][%d]", state, sub_step);
            break;
    }
}

bool IRReceiver::DecodeNEC(const std::vector<uint32_t>& raw_data, uint64_t& command) {
    if (raw_data.size() < 4) {
        return false;
    }
    
    // Check header
    if (!IsDurationInRange(raw_data[0], NEC_HEADER_MARK, NEC_TOLERANCE) ||
        !IsDurationInRange(raw_data[1], NEC_HEADER_SPACE, NEC_TOLERANCE)) {
        return false;
    }
    
    // Decode bits (32 bits for NEC)
    command = 0;
    size_t bit_index = 2;
    
    for (int i = 0; i < 32 && bit_index + 1 < raw_data.size(); i++) {
        uint32_t mark = raw_data[bit_index];
        uint32_t space = raw_data[bit_index + 1];
        
        if (!IsDurationInRange(mark, NEC_BIT_MARK, NEC_TOLERANCE)) {
            return false;
        }
        
        if (IsDurationInRange(space, NEC_ONE_SPACE, NEC_TOLERANCE)) {
            command |= (1ULL << i);
        } else if (!IsDurationInRange(space, NEC_ZERO_SPACE, NEC_TOLERANCE)) {
            return false;
        }
        
        bit_index += 2;
    }
    
    return true;
}

bool IRReceiver::DecodeRC5(const std::vector<uint32_t>& raw_data, uint64_t& command) {
    if (raw_data.size() < 2) {
        return false;
    }
    
    // RC5 uses Manchester encoding
    command = 0;
    size_t bit_index = 0;
    
    for (int i = 0; i < 14 && bit_index + 1 < raw_data.size(); i++) {
        uint32_t duration = raw_data[bit_index] + raw_data[bit_index + 1];
        
        if (IsDurationInRange(duration, RC5_BIT_DURATION, RC5_TOLERANCE)) {
            // Manchester: short then long = 1, long then short = 0
            if (raw_data[bit_index] < raw_data[bit_index + 1]) {
                command |= (1ULL << i);
            }
        } else {
            return false;
        }
        
        bit_index += 2;
    }
    
    return true;
}

bool IRReceiver::DecodeSony(const std::vector<uint32_t>& raw_data, uint64_t& command) {
    if (raw_data.size() < 2) {
        return false;
    }
    
    // Check header
    if (!IsDurationInRange(raw_data[0], SONY_HEADER_MARK, SONY_TOLERANCE) ||
        !IsDurationInRange(raw_data[1], SONY_HEADER_SPACE, SONY_TOLERANCE)) {
        return false;
    }
    
    // Decode bits (12, 15, or 20 bits for Sony)
    command = 0;
    size_t bit_index = 2;
    int bits_to_decode = std::min(20, static_cast<int>(raw_data.size() - 2));
    
    for (int i = 0; i < bits_to_decode && bit_index < raw_data.size(); i++) {
        uint32_t mark = raw_data[bit_index];
        uint32_t space = raw_data[bit_index + 1];
        
        if (!IsDurationInRange(mark, SONY_BIT_MARK, SONY_TOLERANCE)) {
            return false;
        }
        
        if (IsDurationInRange(space, SONY_ONE_SPACE, SONY_TOLERANCE)) {
            command |= (1ULL << i);
        } else if (!IsDurationInRange(space, SONY_ZERO_SPACE, SONY_TOLERANCE)) {
            return false;
        }
        
        bit_index += 2;
    }
    
    return true;
}

bool IRReceiver::IsDurationInRange(uint32_t duration, uint32_t expected, uint32_t tolerance) {
    return (duration >= expected - tolerance) && (duration <= expected + tolerance);
}
