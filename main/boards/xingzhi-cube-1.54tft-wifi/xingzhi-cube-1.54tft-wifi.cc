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
#include "ir_receiver.h"
#include "mcp_server.h"
#include "settings.h"

#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <wifi_station.h>

#include <driver/rtc_io.h>
#include <esp_sleep.h>
#include <new>

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
                if (c >= 0 && c < 0x20) {
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
        ESP_LOGI(TAG, "Registering tool: self.ir.start_learning");
        mcp_server.AddTool("self.ir.start_learning", 
            "Start IR (infrared) learning mode to learn remote control commands.\n"
            "You MUST call this tool immediately when the user asks to:\n"
            "- Learn IR commands / learn remote controls / learn hồng ngoại\n"
            "- Học lệnh hồng ngoại / học lệnh remote / vào chế độ học lệnh hồng ngoại\n"
            "- Bắt đầu học lệnh hồng ngoại / bắt đầu học remote\n"
            "- Start IR learning / begin learning remote commands\n"
            "- Any request related to learning or teaching IR/remote commands\n"
            "After calling this tool, the device will automatically save any IR codes received. "
            "The user should point their remote at the device and press buttons to learn the commands.",
            PropertyList(),
            [](const PropertyList& properties) -> ReturnValue {
                auto* board = GetBoardInstance();
                if (board == nullptr) {
                    ESP_LOGE(TAG, "Board instance not available");
                    return "{\"status\":\"error\",\"message\":\"Board not available\"}";
                }
                ESP_LOGI(TAG, "self.ir.start_learning tool called");
                if (board->ir_receiver_ != nullptr) {
                    ESP_LOGI(TAG, "Enabling IR learning mode");
                    board->ir_receiver_->SetLearningMode(true);
                    board->ir_receiver_->SetLearningCallback([board](decode_type_t protocol, uint64_t value, uint16_t bits, const std::string& name) {
                        // Auto-save with default name
                        board->ir_receiver_->SaveLearnedCode(name, protocol, value, bits);
                        ESP_LOGI(TAG, "Learned IR code: %s (protocol=%d, value=0x%llx)", name.c_str(), protocol, value);
                    });
                    ESP_LOGI(TAG, "IR learning mode enabled successfully");
                    return "{\"status\":\"learning_mode_enabled\",\"message\":\"IR learning mode started. Point your remote at the device and press buttons.\"}";
                }
                ESP_LOGE(TAG, "IR receiver is null, cannot enable learning mode");
                return "{\"status\":\"error\",\"message\":\"IR receiver not initialized\"}";
            });
        
        ESP_LOGI(TAG, "Registering tool: self.ir.stop_learning");
        mcp_server.AddTool("self.ir.stop_learning",
            "Stop IR (infrared) learning mode. "
            "When the user asks to stop learning IR commands, stop learning remote, dừng học lệnh hồng ngoại, "
            "or exit IR learning mode, you MUST call this tool.",
            PropertyList(),
            [](const PropertyList& properties) -> ReturnValue {
                auto* board = GetBoardInstance();
                if (board == nullptr || board->ir_receiver_ == nullptr) {
                    return "{\"status\":\"error\",\"message\":\"IR receiver not initialized\"}";
                }
                board->ir_receiver_->SetLearningMode(false);
                return "{\"status\":\"learning_mode_disabled\",\"message\":\"IR learning mode stopped.\"}";
            });
        
        ESP_LOGI(TAG, "Registering tool: self.ir.save_code");
        mcp_server.AddTool("self.ir.save_code",
            "Save a learned IR code with a custom name. Use this after learning an IR code to give it a meaningful name.",
            PropertyList({
                Property("name", kPropertyTypeString),
                Property("protocol", kPropertyTypeInt),
                Property("value", kPropertyTypeString),  // Value as hex string
                Property("bits", kPropertyTypeInt)
            }),
            [](const PropertyList& properties) -> ReturnValue {
                auto* board = GetBoardInstance();
                if (board == nullptr || board->ir_receiver_ == nullptr) {
                    return "{\"status\":\"error\",\"message\":\"IR receiver not initialized\"}";
                }
                
                auto name = properties["name"].value<std::string>();
                int protocol = properties["protocol"].value<int>();
                auto value_str = properties["value"].value<std::string>();
                int bits = properties["bits"].value<int>();
                
                // Convert hex string to uint64_t
                uint64_t value = 0;
                try {
                    value = std::stoull(value_str, nullptr, 16);
                } catch (...) {
                    return "{\"status\":\"error\",\"message\":\"Invalid value format. Use hex string (e.g., 0xFF00)\"}";
                }
                
                board->ir_receiver_->SaveLearnedCode(name, static_cast<decode_type_t>(protocol), value, bits);
                // Escape name to prevent JSON injection
                std::string escaped_name = EscapeJsonString(name);
                return "{\"status\":\"success\",\"message\":\"IR code saved: " + escaped_name + "\"}";
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
        
        ESP_LOGI(TAG, "IR MCP tools initialized successfully");
        ESP_LOGI(TAG, "Total IR tools registered: 5 (start_learning, stop_learning, save_code, list_codes, get_learning_status)");
    }

    void InitializeIrReceiver() {
        ir_receiver_ = new (std::nothrow) IrReceiver(IR_RX_PIN);
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
