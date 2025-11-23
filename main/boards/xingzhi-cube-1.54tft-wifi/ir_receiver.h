#ifndef __IR_RECEIVER_H__
#define __IR_RECEIVER_H__

#include <driver/gpio.h>
#include <driver/rmt_rx.h>
#include <functional>
#include <vector>
#include <string>

class IRReceiver {
public:
    // Callback function type: void callback(uint64_t command, const std::string& protocol)
    using CommandCallback = std::function<void(uint64_t command, const std::string& protocol)>;
    
    // Callback function type: void callback(const std::vector<uint32_t>& raw_data)
    using RawDataCallback = std::function<void(const std::vector<uint32_t>& raw_data)>;

    IRReceiver(gpio_num_t gpio_num);
    ~IRReceiver();

    // Start receiving IR signals
    void Start();
    
    // Stop receiving IR signals
    void Stop();

    // Check if receiver is running
    bool IsRunning() const { return is_running_; }

    // Set callback for decoded commands
    void OnCommandReceived(CommandCallback callback);

    // Set callback for raw timing data (for learning unknown protocols)
    void OnRawDataReceived(RawDataCallback callback);

    // Get last received command
    uint64_t GetLastCommand() const { return last_command_; }

    // Get last received protocol
    std::string GetLastProtocol() const { return last_protocol_; }

    // Get raw timing data (useful for learning)
    std::vector<uint32_t> GetRawData() const { return raw_data_; }

    // Process received data (call this periodically in main loop)
    void Process();

private:
    gpio_num_t gpio_num_;
    rmt_channel_handle_t rmt_rx_channel_;
    rmt_symbol_word_t* received_symbols_;
    size_t received_symbol_num_;
    bool is_running_;
    
    uint64_t last_command_;
    std::string last_protocol_;
    std::vector<uint32_t> raw_data_;

    CommandCallback command_callback_;
    RawDataCallback raw_data_callback_;
    
    static bool rmt_rx_done_callback(rmt_channel_handle_t channel, const rmt_rx_done_event_data_t* edata, void* user_data);
    void ProcessReceivedData(const rmt_symbol_word_t* symbols, size_t symbol_num);
    
    // Protocol decoders
    bool DecodeNEC(const rmt_symbol_word_t* symbols, size_t symbol_num, uint64_t& command);
    bool DecodeRC5(const rmt_symbol_word_t* symbols, size_t symbol_num, uint64_t& command);
    bool DecodeSony(const rmt_symbol_word_t* symbols, size_t symbol_num, uint64_t& command);
    bool DecodeRaw(const rmt_symbol_word_t* symbols, size_t symbol_num);
    
    // Helper functions
    uint32_t GetSymbolDuration(const rmt_symbol_word_t& symbol);
    bool IsDurationInRange(uint32_t duration, uint32_t expected, uint32_t tolerance);
};

#endif // __IR_RECEIVER_H__
