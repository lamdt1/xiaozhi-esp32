#include "ir_receiver.h"
#include <esp_log.h>
#include <cinttypes>
#include <algorithm>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "IRReceiver"
#define RMT_CLK_DIV 80  // 80MHz / 80 = 1MHz, 1 tick = 1us
#define RMT_RX_FILTER_THRESHOLD 100  // 100us minimum pulse width
#define RMT_RX_IDLE_THRESHOLD 10000  // 10ms idle threshold

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
      rmt_rx_channel_(nullptr),
      received_symbols_(nullptr),
      received_symbol_num_(0),
      is_running_(false),
      last_command_(0),
      last_protocol_("Unknown") {
}

IRReceiver::~IRReceiver() {
    Stop();
    
    // Delete RMT channel in destructor to free resources permanently
    if (rmt_rx_channel_ != nullptr) {
        esp_err_t ret = rmt_del_channel(rmt_rx_channel_);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to delete RMT channel in destructor: %s", esp_err_to_name(ret));
        }
        rmt_rx_channel_ = nullptr;
    }
    
    if (received_symbols_) {
        free(received_symbols_);
        received_symbols_ = nullptr;
    }
}

void IRReceiver::Start() {
    // Reuse existing channel if available and not running
    if (rmt_rx_channel_ != nullptr && !is_running_) {
        // Channel exists but not running - just restart receive
        rmt_receive_config_t receive_cfg = {
            .signal_range_min_ns = RMT_RX_FILTER_THRESHOLD * 1000,
            .signal_range_max_ns = 0x7FFFFFFF,
        };
        
        esp_err_t ret = rmt_enable(rmt_rx_channel_);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to enable existing RMT channel: %s", esp_err_to_name(ret));
            return;
        }
        
        ret = rmt_receive(rmt_rx_channel_, received_symbols_, 
                         sizeof(rmt_symbol_word_t) * 1024, &receive_cfg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start RMT receive on existing channel: %s", esp_err_to_name(ret));
            rmt_disable(rmt_rx_channel_);
            return;
        }
        
        is_running_ = true;
        ESP_LOGI(TAG, "IR Receiver restarted on existing channel (GPIO %d)", gpio_num_);
        return;
    }
    
    if (is_running_ && rmt_rx_channel_ != nullptr) {
        ESP_LOGW(TAG, "IR Receiver already running");
        return;
    }

    // Ensure any previous channel is fully cleaned up
    if (rmt_rx_channel_ != nullptr || is_running_) {
        ESP_LOGW(TAG, "Cleaning up existing RMT channel before starting");
        Stop();
        // Additional delay to ensure RMT channel is fully released
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    
    // If channel still exists after cleanup, force delete it to free resources
    if (rmt_rx_channel_ != nullptr) {
        ESP_LOGW(TAG, "Force deleting RMT channel to free resources");
        // Disable first if not already disabled
        rmt_disable(rmt_rx_channel_);
        vTaskDelay(pdMS_TO_TICKS(50));
        
        esp_err_t ret = rmt_del_channel(rmt_rx_channel_);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to delete RMT channel: %s", esp_err_to_name(ret));
        } else {
            ESP_LOGI(TAG, "RMT channel deleted successfully");
        }
        rmt_rx_channel_ = nullptr;
        // Additional delay after deletion to allow RMT driver to fully release resources
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    // Configure RMT receiver
    rmt_rx_channel_config_t rx_channel_cfg = {
        .gpio_num = gpio_num_,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 1000000,  // 1MHz = 1us per tick
        .mem_block_symbols = 1024,
        .flags = {
            .invert_in = false,
            .with_dma = false,
        }
    };
    
    esp_err_t ret = rmt_new_rx_channel(&rx_channel_cfg, &rmt_rx_channel_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create RMT RX channel: %s (0x%x)", esp_err_to_name(ret), ret);
        rmt_rx_channel_ = nullptr;
        return;
    }
    
    // Register callback first
    rmt_rx_event_callbacks_t cbs = {
        .on_recv_done = rmt_rx_done_callback,
    };
    ret = rmt_rx_register_event_callbacks(rmt_rx_channel_, &cbs, this);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register RMT callbacks: %s", esp_err_to_name(ret));
        rmt_del_channel(rmt_rx_channel_);
        rmt_rx_channel_ = nullptr;
        return;
    }
    
    // Allocate buffer for received symbols
    if (!received_symbols_) {
        received_symbols_ = (rmt_symbol_word_t*)malloc(sizeof(rmt_symbol_word_t) * 1024);
        if (!received_symbols_) {
            ESP_LOGE(TAG, "Failed to allocate memory for RMT symbols");
            rmt_del_channel(rmt_rx_channel_);
            rmt_rx_channel_ = nullptr;
            return;
        }
    }
    
    // Set receive filter
    rmt_receive_config_t receive_cfg = {
        .signal_range_min_ns = RMT_RX_FILTER_THRESHOLD * 1000,
        .signal_range_max_ns = 0x7FFFFFFF,
    };
    
    ret = rmt_enable(rmt_rx_channel_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable RMT channel: %s", esp_err_to_name(ret));
        rmt_del_channel(rmt_rx_channel_);
        rmt_rx_channel_ = nullptr;
        return;
    }
    
    ret = rmt_receive(rmt_rx_channel_, received_symbols_, 
                     sizeof(rmt_symbol_word_t) * 1024, &receive_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start RMT receive: %s", esp_err_to_name(ret));
        rmt_disable(rmt_rx_channel_);
        rmt_del_channel(rmt_rx_channel_);
        rmt_rx_channel_ = nullptr;
        return;
    }
    
    is_running_ = true;
    ESP_LOGI(TAG, "IR Receiver started on GPIO %d", gpio_num_);
}

void IRReceiver::Stop() {
    if (!is_running_ && rmt_rx_channel_ == nullptr) {
        return;
    }

    // Mark as stopped first to prevent callback from restarting receive
    is_running_ = false;

    if (rmt_rx_channel_) {
        // Disable channel first (but don't delete - reuse it next time)
        esp_err_t ret = rmt_disable(rmt_rx_channel_);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to disable RMT channel: %s", esp_err_to_name(ret));
        }
        
        // Small delay to ensure any pending callback operations complete
        vTaskDelay(pdMS_TO_TICKS(10));
        
        // Note: We keep the channel handle to reuse it next time
        // Only delete in destructor to free resources permanently
    }
    
    ESP_LOGI(TAG, "IR Receiver stopped (channel kept for reuse)");
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

bool IRReceiver::rmt_rx_done_callback(rmt_channel_handle_t channel, const rmt_rx_done_event_data_t* edata, void* user_data) {
    IRReceiver* receiver = static_cast<IRReceiver*>(user_data);
    if (receiver && edata && receiver->is_running_ && receiver->rmt_rx_channel_ != nullptr) {
        receiver->ProcessReceivedData(edata->received_symbols, edata->num_symbols);
        
        // Restart receiving only if receiver is still running
        // Check again after processing (receiver might have been stopped)
        if (receiver->is_running_ && receiver->rmt_rx_channel_ != nullptr) {
            rmt_receive_config_t receive_cfg = {
                .signal_range_min_ns = RMT_RX_FILTER_THRESHOLD * 1000,
                .signal_range_max_ns = 0x7FFFFFFF,
            };
            esp_err_t ret = rmt_receive(receiver->rmt_rx_channel_, receiver->received_symbols_, 
                        sizeof(rmt_symbol_word_t) * 1024, &receive_cfg);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Failed to restart RMT receive in callback: %s", esp_err_to_name(ret));
                receiver->is_running_ = false;  // Mark as stopped if restart fails
            }
        }
    }
    return false; // Return false to indicate we don't want to free the buffer
}

void IRReceiver::ProcessReceivedData(const rmt_symbol_word_t* symbols, size_t symbol_num) {
    if (!symbols || symbol_num == 0) {
        return;
    }

    // Store raw data - store duration0 and duration1 separately for each symbol
    // This matches the DecodeRaw implementation and allows proper reconstruction of IR signals
    raw_data_.clear();
    raw_data_.reserve(symbol_num * 2);
    for (size_t i = 0; i < symbol_num; i++) {
        raw_data_.push_back(symbols[i].duration0);
        raw_data_.push_back(symbols[i].duration1);
    }

    // Try to decode different protocols
    uint64_t command = 0;
    std::string protocol = "UNKNOWN";
    
    if (DecodeNEC(symbols, symbol_num, command)) {
        protocol = "NEC";
    } else if (DecodeRC5(symbols, symbol_num, command)) {
        protocol = "RC5";
    } else if (DecodeSony(symbols, symbol_num, command)) {
        protocol = "Sony";
    } else {
        // Raw data for unknown protocols
        DecodeRaw(symbols, symbol_num);
        protocol = "Raw";
    }

    // Update last received data
    last_command_ = command;
    last_protocol_ = protocol;

    ESP_LOGI(TAG, "IR Command received: %s - 0x%" PRIX64 " (%zu symbols)", 
             protocol.c_str(), command, symbol_num);

    // Call callback if set
    if (command_callback_) {
        command_callback_(command, protocol);
    }
}

uint32_t IRReceiver::GetSymbolDuration(const rmt_symbol_word_t& symbol) {
    return symbol.duration0 + symbol.duration1;
}

bool IRReceiver::IsDurationInRange(uint32_t duration, uint32_t expected, uint32_t tolerance) {
    return (duration >= expected - tolerance) && (duration <= expected + tolerance);
}

bool IRReceiver::DecodeNEC(const rmt_symbol_word_t* symbols, size_t symbol_num, uint64_t& command) {
    if (symbol_num < 68) {  // NEC needs at least 34 pairs (68 symbols)
        return false;
    }

    // Check header
    if (!IsDurationInRange(symbols[0].duration0, NEC_HEADER_MARK, NEC_TOLERANCE) ||
        !IsDurationInRange(symbols[0].duration1, NEC_HEADER_SPACE, NEC_TOLERANCE)) {
        return false;
    }

    // Decode 32 bits
    command = 0;
    for (size_t i = 1; i < 33 && i < symbol_num; i++) {
        if (!IsDurationInRange(symbols[i].duration0, NEC_BIT_MARK, NEC_TOLERANCE)) {
            return false;
        }
        
        command <<= 1;
        if (IsDurationInRange(symbols[i].duration1, NEC_ONE_SPACE, NEC_TOLERANCE)) {
            command |= 1;
        } else if (!IsDurationInRange(symbols[i].duration1, NEC_ZERO_SPACE, NEC_TOLERANCE)) {
            return false;
        }
    }

    return true;
}

bool IRReceiver::DecodeRC5(const rmt_symbol_word_t* symbols, size_t symbol_num, uint64_t& command) {
    if (symbol_num < 26) {  // RC5 needs at least 13 bits (26 symbols)
        return false;
    }

    // RC5 uses Manchester encoding
    command = 0;
    for (size_t i = 0; i < 13 && i * 2 + 1 < symbol_num; i++) {
        uint32_t first = GetSymbolDuration(symbols[i * 2]);
        uint32_t second = GetSymbolDuration(symbols[i * 2 + 1]);
        
        if (IsDurationInRange(first, RC5_BIT_DURATION, RC5_TOLERANCE) &&
            IsDurationInRange(second, RC5_BIT_DURATION, RC5_TOLERANCE)) {
            // Manchester: 1 = low-high, 0 = high-low
            command <<= 1;
            if (symbols[i * 2].level0 == 0) {  // Low first = 1
                command |= 1;
            }
        } else {
            return false;
        }
    }

    return true;
}

bool IRReceiver::DecodeSony(const rmt_symbol_word_t* symbols, size_t symbol_num, uint64_t& command) {
    if (symbol_num < 24) {  // Sony needs at least 12 bits (24 symbols)
        return false;
    }

    // Check header
    if (!IsDurationInRange(symbols[0].duration0, SONY_HEADER_MARK, SONY_TOLERANCE) ||
        !IsDurationInRange(symbols[0].duration1, SONY_HEADER_SPACE, SONY_TOLERANCE)) {
        return false;
    }

    // Decode bits (12, 15, or 20 bits)
    // Sony protocol: 1 symbol per bit (symbol[0] is header, symbols[1..N] are data bits)
    // Each symbol contains: duration0 = mark, duration1 = space
    command = 0;
    size_t bits_to_decode = std::min(symbol_num - 1, size_t(20));  // -1 for header symbol
    
    for (size_t i = 1; i <= bits_to_decode && i < symbol_num; i++) {
        if (!IsDurationInRange(symbols[i].duration0, SONY_BIT_MARK, SONY_TOLERANCE)) {
            return false;
        }
        
        command <<= 1;
        if (IsDurationInRange(symbols[i].duration1, SONY_ONE_SPACE, SONY_TOLERANCE)) {
            command |= 1;
        } else if (!IsDurationInRange(symbols[i].duration1, SONY_ZERO_SPACE, SONY_TOLERANCE)) {
            return false;
        }
    }

    return true;
}

bool IRReceiver::DecodeRaw(const rmt_symbol_word_t* symbols, size_t symbol_num) {
    // Just store raw timing data
    raw_data_.clear();
    raw_data_.reserve(symbol_num * 2);
    for (size_t i = 0; i < symbol_num; i++) {
        raw_data_.push_back(symbols[i].duration0);
        raw_data_.push_back(symbols[i].duration1);
    }
    return true;
}
