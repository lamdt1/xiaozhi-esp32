#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "zhengchen_lcd_display.h"
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

#define TAG "ZHENGCHEN_1_54TFT_WIFI"

class ZHENGCHEN_1_54TFT_WIFI : public WifiBoard {
private:
    Button boot_button_;
    Button volume_up_button_;
    Button volume_down_button_;
    ZHENGCHEN_LcdDisplay* display_;
    PowerSaveTimer* power_save_timer_;
    PowerManager* power_manager_;
    IrReceiver* ir_receiver_;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;

    void InitializePowerManager() {
        power_manager_ = new PowerManager(GPIO_NUM_9);
        power_manager_->OnTemperatureChanged([this](float chip_temp) {
            display_->UpdateHighTempWarning(chip_temp);
        });

        power_manager_->OnChargingStatusChanged([this](bool is_charging) {
            if (is_charging) {
                power_save_timer_->SetEnabled(false);
                ESP_LOGI("PowerManager", "Charging started");
            } else {
                power_save_timer_->SetEnabled(true);
                ESP_LOGI("PowerManager", "Charging stopped");
            }
        });
    
    }

    void InitializePowerSaveTimer() {
        rtc_gpio_init(GPIO_NUM_2);
        rtc_gpio_set_direction(GPIO_NUM_2, RTC_GPIO_MODE_OUTPUT_ONLY);
        rtc_gpio_set_level(GPIO_NUM_2, 1);

        power_save_timer_ = new PowerSaveTimer(-1, 60, 300);
        power_save_timer_->OnEnterSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(true);
            GetBacklight()->SetBrightness(1);
        });
        power_save_timer_->OnExitSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(false);
            GetBacklight()->RestoreBrightness();
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

        // 设置开机按钮的长按事件（直接进入配网模式）
        boot_button_.OnLongPress([this]() {
            // 唤醒电源保存定时器
            power_save_timer_->WakeUp();
            // 获取应用程序实例
            auto& app = Application::GetInstance();
            
            // 进入配网模式
            app.SetDeviceState(kDeviceStateWifiConfiguring);
            
            // 重置WiFi配置以确保进入配网模式
            ResetWifiConfiguration();
        });

        volume_up_button_.OnClick([this]() {
            power_save_timer_->WakeUp();
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() + 10;
            if (volume > 100) {
                volume = 100;
            }
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume/10));
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
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume/10));
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

        display_ = new ZHENGCHEN_LcdDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, 
            DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
        display_->SetupHighTempWarningPopup();
    }

    void InitializeTools() {
        auto& mcp_server = McpServer::GetInstance();
        
        // IR Learning Mode Control
        mcp_server.AddTool("self.ir.start_learning", 
            "Start IR learning mode. When enabled, the device will save any IR codes received. "
            "Use this to learn IR commands from remote controls.",
            PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                if (ir_receiver_ != nullptr) {
                    ir_receiver_->SetLearningMode(true);
                    ir_receiver_->SetLearningCallback([this](decode_type_t protocol, uint64_t value, uint16_t bits, const std::string& name) {
                        // Auto-save with default name
                        ir_receiver_->SaveLearnedCode(name, protocol, value, bits);
                        ESP_LOGI(TAG, "Learned IR code: %s (protocol=%d, value=0x%llx)", name.c_str(), protocol, value);
                    });
                    return "{\"status\":\"learning_mode_enabled\",\"message\":\"IR learning mode started. Point your remote at the device and press buttons.\"}";
                }
                return "{\"status\":\"error\",\"message\":\"IR receiver not initialized\"}";
            });
        
        mcp_server.AddTool("self.ir.stop_learning",
            "Stop IR learning mode.",
            PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                if (ir_receiver_ != nullptr) {
                    ir_receiver_->SetLearningMode(false);
                    return "{\"status\":\"learning_mode_disabled\",\"message\":\"IR learning mode stopped.\"}";
                }
                return "{\"status\":\"error\",\"message\":\"IR receiver not initialized\"}";
            });
        
        mcp_server.AddTool("self.ir.save_code",
            "Save a learned IR code with a custom name. Use this after learning an IR code to give it a meaningful name.",
            PropertyList({
                Property("name", kPropertyTypeString),
                Property("protocol", kPropertyTypeInt),
                Property("value", kPropertyTypeString),  // Value as hex string
                Property("bits", kPropertyTypeInt)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                if (ir_receiver_ == nullptr) {
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
                
                ir_receiver_->SaveLearnedCode(name, static_cast<decode_type_t>(protocol), value, bits);
                return "{\"status\":\"success\",\"message\":\"IR code saved: " + name + "\"}";
            });
        
        mcp_server.AddTool("self.ir.list_codes",
            "List all learned IR codes.",
            PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                if (ir_receiver_ != nullptr) {
                    return ir_receiver_->GetLearnedCodes();
                }
                return "{\"codes\":[]}";
            });
        
        mcp_server.AddTool("self.ir.get_learning_status",
            "Get the current status of IR learning mode.",
            PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                if (ir_receiver_ != nullptr) {
                    bool learning = ir_receiver_->IsLearningMode();
                    return learning ? "{\"learning_mode\":true}" : "{\"learning_mode\":false}";
                }
                return "{\"learning_mode\":false,\"error\":\"IR receiver not initialized\"}";
            });
    }

    void InitializeIrReceiver() {
        ir_receiver_ = new IrReceiver(IR_RX_PIN);
        if (ir_receiver_ == nullptr) {
            ESP_LOGE(TAG, "Failed to create IR receiver");
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
    ZHENGCHEN_1_54TFT_WIFI() :
        boot_button_(BOOT_BUTTON_GPIO),
        volume_up_button_(VOLUME_UP_BUTTON_GPIO),
        volume_down_button_(VOLUME_DOWN_BUTTON_GPIO),
        ir_receiver_(nullptr) {
        InitializePowerManager();
        InitializePowerSaveTimer();
        InitializeSpi();
        InitializeButtons();
        InitializeSt7789Display();  
        InitializeTools();
        InitializeIrReceiver();
        GetBacklight()->RestoreBrightness();
    }

    ~ZHENGCHEN_1_54TFT_WIFI() {
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

    // 获取音频编解码器
    virtual AudioCodec* GetAudioCodec() override {
        // 静态实例化NoAudioCodecSimplex类
        static NoAudioCodecSimplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT, AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
        // 返回音频编解码器
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
        level = std::max<uint32_t>(power_manager_->GetBatteryLevel(), 20);
        return true;
    }

    virtual bool GetTemperature(float& esp32temp)  override {
        esp32temp = power_manager_->GetTemperature();
        return true;
    }

    virtual void SetPowerSaveMode(bool enabled) override {
        if (!enabled) {
            power_save_timer_->WakeUp();
        }
        WifiBoard::SetPowerSaveMode(enabled);
    }
};

DECLARE_BOARD(ZHENGCHEN_1_54TFT_WIFI);
