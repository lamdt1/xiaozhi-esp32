#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "display/lcd_display.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "power_save_timer.h"
#include "led/single_led.h"
#include "assets/lang_config.h"
#include "power_manager.h"
#include "mcp_server.h"
#include "settings.h"

#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <wifi_station.h>

#include <driver/rtc_io.h>
#include <esp_sleep.h>
#include <new>

// Forward declaration to avoid IPADDR_NONE conflict between lwip and Arduino headers
class IrReceiver;

// Include ir_receiver.h after all system headers to avoid IPADDR_NONE conflict with lwip
#include "ir_receiver.h"

#define TAG "XINGZHI_CUBE_1_54TFT_WIFI"

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

class XINGZHI_CUBE_1_54TFT_WIFI : public WifiBoard {
private:
    Button boot_button_;
    Button volume_up_button_;
    Button volume_down_button_;
    SpiLcdDisplay* display_;
    PowerSaveTimer* power_save_timer_;
    PowerManager* power_manager_;
    IrReceiver* ir_receiver_;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;

    // Static helper to safely get board instance (avoids dangling pointer in callbacks)
    static XINGZHI_CUBE_1_54TFT_WIFI* GetBoardInstance() {
        return static_cast<XINGZHI_CUBE_1_54TFT_WIFI*>(&Board::GetInstance());
    }

    void InitializePowerManager() {
        power_manager_ = new PowerManager(GPIO_NUM_38);
        power_manager_->OnChargingStatusChanged([this](bool is_charging) {
            if (is_charging) {
                power_save_timer_->SetEnabled(false);
            } else {
                power_save_timer_->SetEnabled(true);
            }
        });
    }

    void InitializePowerSaveTimer() {
        rtc_gpio_init(GPIO_NUM_21);
        rtc_gpio_set_direction(GPIO_NUM_21, RTC_GPIO_MODE_OUTPUT_ONLY);
        rtc_gpio_set_level(GPIO_NUM_21, 1);

        power_save_timer_ = new PowerSaveTimer(-1, 60, 300);
        power_save_timer_->OnEnterSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(true);
            GetBacklight()->SetBrightness(1);
        });
        power_save_timer_->OnExitSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(false);
            GetBacklight()->RestoreBrightness();
        });
        power_save_timer_->OnShutdownRequest([this]() {
            ESP_LOGI(TAG, "Shutting down");
            rtc_gpio_set_level(GPIO_NUM_21, 0);
            // 启用保持功能，确保睡眠期间电平不变
            rtc_gpio_hold_en(GPIO_NUM_21);
            esp_lcd_panel_disp_on_off(panel_, false); //关闭显示
            esp_deep_sleep_start();
        });
        power_save_timer_->SetEnabled(true);
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_SDA;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_SCL;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            power_save_timer_->WakeUp();
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting && !WifiStation::GetInstance().IsConnected()) {
                ResetWifiConfiguration();
            }
            app.ToggleChatState();
        });

        volume_up_button_.OnClick([this]() {
            power_save_timer_->WakeUp();
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() + 10;
            if (volume > 100) {
                volume = 100;
            }
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        });

        volume_up_button_.OnLongPress([this]() {
            power_save_timer_->WakeUp();
            GetAudioCodec()->SetOutputVolume(100);
            GetDisplay()->ShowNotification(Lang::Strings::MAX_VOLUME);
        });

        volume_down_button_.OnClick([this]() {
            power_save_timer_->WakeUp();
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() - 10;
            if (volume < 0) {
                volume = 0;
            }
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        });

        volume_down_button_.OnLongPress([this]() {
            power_save_timer_->WakeUp();
            GetAudioCodec()->SetOutputVolume(0);
            GetDisplay()->ShowNotification(Lang::Strings::MUTED);
        });
    }

    void InitializeSt7789Display() {
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS;
        io_config.dc_gpio_num = DISPLAY_DC;
        io_config.spi_mode = 3;
        io_config.pclk_hz = 80 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io_));

        ESP_LOGD(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RES;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io_, &panel_config, &panel_));
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel_));
        ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_, DISPLAY_SWAP_XY));
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y));
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_, true));

        display_ = new SpiLcdDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, 
            DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

    void InitializeTools() {
        ESP_LOGI(TAG, "=== InitializeTools() called ===");
        ESP_LOGI(TAG, "Board type: xingzhi-cube-1.54tft-wifi");
        auto& mcp_server = McpServer::GetInstance();
        ESP_LOGI(TAG, "MCP server instance obtained");
        
        ESP_LOGI(TAG, "Initializing IR MCP tools...");
        ESP_LOGI(TAG, "IR receiver pointer: %p", ir_receiver_);
        
        // IR Learning Mode Control
        ESP_LOGI(TAG, "Registering tool: self.ir.learn_code");
        mcp_server.AddTool("self.ir.learn_code", 
            "Learn a single IR (infrared) code and save it with a specific name. (Học một lệnh hồng ngoại và lưu với tên cụ thể).\n"
            "Use this when the user wants to learn or save a new remote command with a name like 'TV on' or 'Fan speed up'.\n"
            "You MUST provide a 'name' for the command.\n"
            "Example: self.ir.learn_code(name=\"tv_power\")",
            PropertyList({
                Property("name", kPropertyTypeString)
            }),
            [](const PropertyList& properties) -> ReturnValue {
                auto* board = GetBoardInstance();
                if (board == nullptr || board->ir_receiver_ == nullptr) {
                    return "{\"status\":\"error\",\"message\":\"IR receiver not initialized\"}";
                }

                auto name = properties["name"].value<std::string>();
                if (name.empty()) {
                    return "{\"status\":\"error\",\"message\":\"Command name cannot be empty\"}";
                }
                // NVS Key length is 15 chars, "code_" prefix is 5, so name is max 10.
                if (name.length() > 10) {
                    return "{\"status\":\"error\",\"message\":\"Name is too long (max 10 characters)\"}";
                }

                ESP_LOGI(TAG, "Starting one-shot learn for command: %s", name.c_str());

                // Check if learning mode is already active
                if (board->ir_receiver_->IsLearningMode()) {
                    ESP_LOGW(TAG, "Learning mode already active, will replace existing callback");
                }

                // Set a one-shot learning callback for protocol-based codes
                board->ir_receiver_->SetLearningCallback([name](decode_type_t protocol, uint64_t value, uint16_t bits, const std::string& default_name) {
                    auto* board_cb = GetBoardInstance();
                    if (board_cb != nullptr && board_cb->ir_receiver_ != nullptr) {
                        // Save the code with the user-provided name
                        board_cb->ir_receiver_->SaveLearnedCode(name, protocol, value, bits);
                        ESP_LOGI(TAG, "Learned and saved IR code '%s'", name.c_str());
                        
                        // Immediately disable learning mode after capture
                        board_cb->ir_receiver_->SetLearningMode(false);
                        // Clear the callback so it doesn't fire again
                        board_cb->ir_receiver_->SetLearningCallback(nullptr);
                        board_cb->ir_receiver_->SetRawLearningCallback(nullptr);
                    }
                });
                
                // Also set raw learning callback to save raw data (works even for invalid protocols)
                board->ir_receiver_->SetRawLearningCallback([name](const uint16_t* raw_data, uint16_t raw_len, const std::string& default_name) {
                    auto* board_cb = GetBoardInstance();
                    if (board_cb != nullptr && board_cb->ir_receiver_ != nullptr) {
                        // Save raw data with the user-provided name
                        board_cb->ir_receiver_->SaveRawCode(name, raw_data, raw_len);
                        ESP_LOGI(TAG, "Learned and saved raw IR code '%s' (raw_len=%u)", name.c_str(), raw_len);
                        
                        // Disable learning mode after saving raw data (one-shot)
                        board_cb->ir_receiver_->SetLearningMode(false);
                        // Clear callbacks so they don't fire again
                        board_cb->ir_receiver_->SetLearningCallback(nullptr);
                        board_cb->ir_receiver_->SetRawLearningCallback(nullptr);
                    }
                });

                // Enable learning mode to capture the next code
                board->ir_receiver_->SetLearningMode(true);
                
                std::string escaped_name = EscapeJsonString(name);
                return "{\"status\":\"learning\",\"message\":\"Ready to learn code for '" + escaped_name + "'. Press a button on your remote now.\"}";
            });
        
        ESP_LOGI(TAG, "Registering tool: self.ir.list_codes");
        mcp_server.AddTool("self.ir.list_codes",
            "List all learned IR (infrared) codes that have been saved. "
            "When the user asks to see learned IR codes, list remote commands, xem danh sách lệnh hồng ngoại, "
            "or show learned codes, you MUST call this tool.",
            PropertyList(),
            [](const PropertyList& properties) -> ReturnValue {
                auto* board = GetBoardInstance();
                if (board != nullptr && board->ir_receiver_ != nullptr) {
                    return board->ir_receiver_->GetLearnedCodes();
                }
                return "{\"codes\":[]}";
            });
        
        ESP_LOGI(TAG, "Registering tool: self.ir.get_learning_status");
        mcp_server.AddTool("self.ir.get_learning_status",
            "Get the current status of IR learning mode.",
            PropertyList(),
            [](const PropertyList& properties) -> ReturnValue {
                auto* board = GetBoardInstance();
                if (board != nullptr && board->ir_receiver_ != nullptr) {
                    bool learning = board->ir_receiver_->IsLearningMode();
                    return learning ? "{\"learning_mode\":true}" : "{\"learning_mode\":false}";
                }
                return "{\"learning_mode\":false,\"error\":\"IR receiver not initialized\"}";
            });
        
        ESP_LOGI(TAG, "Registering tool: self.ir.delete_code");
        mcp_server.AddTool("self.ir.delete_code",
            "Delete a learned IR (infrared) code by name. "
            "When the user asks to delete an IR code, remove a learned code, xóa lệnh hồng ngoại, "
            "or delete a remote command, you MUST call this tool.",
            PropertyList({
                Property("name", kPropertyTypeString)
            }),
            [](const PropertyList& properties) -> ReturnValue {
                auto* board = GetBoardInstance();
                if (board == nullptr || board->ir_receiver_ == nullptr) {
                    return "{\"status\":\"error\",\"message\":\"IR receiver not initialized\"}";
                }
                
                auto name = properties["name"].value<std::string>();
                if (name.empty()) {
                    return "{\"status\":\"error\",\"message\":\"Code name cannot be empty\"}";
                }
                
                bool deleted = board->ir_receiver_->DeleteLearnedCode(name);
                if (deleted) {
                    std::string escaped_name = EscapeJsonString(name);
                    return "{\"status\":\"success\",\"message\":\"IR code deleted: " + escaped_name + "\"}";
                } else {
                    std::string escaped_name = EscapeJsonString(name);
                    return "{\"status\":\"error\",\"message\":\"IR code not found: " + escaped_name + "\"}";
                }
            });
        
        ESP_LOGI(TAG, "Registering tool: self.ir.delete_all_codes");
        mcp_server.AddTool("self.ir.delete_all_codes",
            "Delete all learned IR (infrared) codes. "
            "When the user asks to delete all IR codes, clear all learned codes, xóa hết lệnh hồng ngoại, "
            "xóa tất cả lệnh đã học, reset IR codes, or start fresh, you MUST call this tool.",
            PropertyList(),
            [](const PropertyList& properties) -> ReturnValue {
                auto* board = GetBoardInstance();
                if (board == nullptr || board->ir_receiver_ == nullptr) {
                    return "{\"status\":\"error\",\"message\":\"IR receiver not initialized\"}";
                }
                
                board->ir_receiver_->DeleteAllLearnedCodes();
                return "{\"status\":\"success\",\"message\":\"All IR codes deleted. You can now learn new codes from scratch.\"}";
            });
        
        ESP_LOGI(TAG, "Registering tool: self.ir.send_code");
        mcp_server.AddTool("self.ir.send_code",
            "Send/transmit a learned IR (infrared) code by name. "
            "When the user wants to send an IR command, transmit an IR code, gửi lệnh hồng ngoại, "
            "or control a device via IR, you MUST call this tool. "
            "This will try to send as protocol-based code first, then try raw data if available.",
            PropertyList({
                Property("name", kPropertyTypeString)
            }),
            [](const PropertyList& properties) -> ReturnValue {
                auto* board = GetBoardInstance();
                if (board == nullptr || board->ir_receiver_ == nullptr) {
                    return "{\"status\":\"error\",\"message\":\"IR receiver not initialized\"}";
                }
                
                auto name = properties["name"].value<std::string>();
                if (name.empty()) {
                    return "{\"status\":\"error\",\"message\":\"Code name cannot be empty\"}";
                }
                
                // SendLearnedCode now automatically tries raw data as fallback
                bool sent = board->ir_receiver_->SendLearnedCode(name);
                
                if (sent) {
                    std::string escaped_name = EscapeJsonString(name);
                    return "{\"status\":\"success\",\"message\":\"IR code sent: " + escaped_name + "\"}";
                } else {
                    std::string escaped_name = EscapeJsonString(name);
                    return "{\"status\":\"error\",\"message\":\"Failed to send IR code: " + escaped_name + "\"}";
                }
            });
        
        ESP_LOGI(TAG, "Registering tool: self.ir.send_raw_code");
        mcp_server.AddTool("self.ir.send_raw_code",
            "Send/transmit a learned raw IR (infrared) code by name. "
            "Use this when you want to send raw IR data that was saved (works for any protocol, even invalid ones).",
            PropertyList({
                Property("name", kPropertyTypeString)
            }),
            [](const PropertyList& properties) -> ReturnValue {
                auto* board = GetBoardInstance();
                if (board == nullptr || board->ir_receiver_ == nullptr) {
                    return "{\"status\":\"error\",\"message\":\"IR receiver not initialized\"}";
                }
                
                auto name = properties["name"].value<std::string>();
                if (name.empty()) {
                    return "{\"status\":\"error\",\"message\":\"Code name cannot be empty\"}";
                }
                
                bool sent = board->ir_receiver_->SendLearnedRawCode(name);
                if (sent) {
                    std::string escaped_name = EscapeJsonString(name);
                    return "{\"status\":\"success\",\"message\":\"Raw IR code sent: " + escaped_name + "\"}";
                } else {
                    std::string escaped_name = EscapeJsonString(name);
                    return "{\"status\":\"error\",\"message\":\"Failed to send raw IR code: " + escaped_name + "\"}";
                }
            });
        
        ESP_LOGI(TAG, "Registering tool: self.ir.export_constants");
        mcp_server.AddTool("self.ir.export_constants",
            "Export all learned IR codes as C++ constants that can be used in code. "
            "When the user wants to export IR codes as constants, generate C++ code, "
            "or create header file with IR commands, you MUST call this tool.",
            PropertyList(),
            [](const PropertyList& properties) -> ReturnValue {
                auto* board = GetBoardInstance();
                if (board == nullptr || board->ir_receiver_ == nullptr) {
                    return "{\"status\":\"error\",\"message\":\"IR receiver not initialized\"}";
                }
                
                std::string constants = board->ir_receiver_->ExportAsConstants();
                std::string escaped = EscapeJsonString(constants);
                return "{\"status\":\"success\",\"constants\":\"" + escaped + "\"}";
            });
        
        ESP_LOGI(TAG, "IR MCP tools initialized successfully");
        ESP_LOGI(TAG, "Total IR tools registered: 8 (learn_code, list_codes, get_learning_status, delete_code, delete_all_codes, send_code, send_raw_code, export_constants)");
    }

    void InitializeIrReceiver() {
        ir_receiver_ = new (std::nothrow) IrReceiver(IR_RX_PIN, IR_TX_PIN);
        if (ir_receiver_ == nullptr) {
            ESP_LOGE(TAG, "Failed to create IR receiver (out of memory)");
            return;
        }
        
        // Set callback to handle IR commands
        ir_receiver_->SetCallback([this](decode_type_t protocol, uint64_t value, uint16_t bits) {
            ESP_LOGI(TAG, "IR command received: protocol=%d, value=0x%llx", protocol, value);
            
            // Wake up from power save mode when IR command is received
            if (power_save_timer_ != nullptr) {
                power_save_timer_->WakeUp();
            }
            
            // You can add custom IR command handling here
            // For example, map IR codes to volume control, etc.
        });
        
        // Start the IR receiver
        ir_receiver_->Start();
        ESP_LOGI(TAG, "IR receiver initialized and started");
    }

public:
    XINGZHI_CUBE_1_54TFT_WIFI() :
        boot_button_(BOOT_BUTTON_GPIO),
        volume_up_button_(VOLUME_UP_BUTTON_GPIO),
        volume_down_button_(VOLUME_DOWN_BUTTON_GPIO),
        display_(nullptr),
        power_save_timer_(nullptr),
        power_manager_(nullptr),
        ir_receiver_(nullptr) {
        ESP_LOGI(TAG, "=== XINGZHI_CUBE_1_54TFT_WIFI constructor started ===");
        InitializePowerManager();
        InitializePowerSaveTimer();
        InitializeSpi();
        InitializeButtons();
        InitializeSt7789Display();
        ESP_LOGI(TAG, "About to initialize IR receiver...");
        InitializeIrReceiver();  // Initialize IR receiver BEFORE tools so it's available
        ESP_LOGI(TAG, "About to initialize tools...");
        InitializeTools();
        ESP_LOGI(TAG, "Tools initialization completed");
        GetBacklight()->RestoreBrightness();
        ESP_LOGI(TAG, "=== XINGZHI_CUBE_1_54TFT_WIFI constructor completed ===");
    }

    ~XINGZHI_CUBE_1_54TFT_WIFI() {
        // Clean up IR receiver
        if (ir_receiver_ != nullptr) {
            delete ir_receiver_;
            ir_receiver_ = nullptr;
        }
        
        // Clean up other resources
        if (power_save_timer_ != nullptr) {
            delete power_save_timer_;
            power_save_timer_ = nullptr;
        }
        
        if (power_manager_ != nullptr) {
            delete power_manager_;
            power_manager_ = nullptr;
        }
        
        if (display_ != nullptr) {
            delete display_;
            display_ = nullptr;
        }
    }

    virtual AudioCodec* GetAudioCodec() override {
        static NoAudioCodecSimplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT, AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }
    
    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        static bool last_discharging = false;
        charging = power_manager_->IsCharging();
        discharging = power_manager_->IsDischarging();
        if (discharging != last_discharging) {
            power_save_timer_->SetEnabled(discharging);
            last_discharging = discharging;
        }
        level = power_manager_->GetBatteryLevel();
        return true;
    }

    virtual void SetPowerSaveMode(bool enabled) override {
        if (!enabled) {
            power_save_timer_->WakeUp();
        }
        WifiBoard::SetPowerSaveMode(enabled);
    }
};

DECLARE_BOARD(XINGZHI_CUBE_1_54TFT_WIFI);
