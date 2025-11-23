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

#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <wifi_station.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <freertos/queue.h>

#include <driver/rtc_io.h>
#include <esp_sleep.h>
#include <cJSON.h>
#include <cinttypes>

#define TAG "XINGZHI_CUBE_1_54TFT_WIFI"

class XINGZHI_CUBE_1_54TFT_WIFI : public WifiBoard {
private:
    Button boot_button_;
    Button volume_up_button_;
    Button volume_down_button_;
    SpiLcdDisplay* display_;
    PowerSaveTimer* power_save_timer_;
    PowerManager* power_manager_;
    IRReceiver* ir_receiver_;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    
    // IR learning state
    SemaphoreHandle_t ir_learn_semaphore_ = nullptr;
    QueueHandle_t ir_command_queue_ = nullptr;  // Queue to defer callback from ISR
    TaskHandle_t ir_learn_task_handle_ = nullptr;  // Task handle for cleanup
    bool ir_learning_active_ = false;
    uint64_t learned_command_ = 0;
    std::string learned_protocol_ = "";
    std::vector<uint32_t> learned_raw_data_;
    
    struct IRCommandEvent {
        uint64_t command;
        std::string protocol;
        std::vector<uint32_t> raw_data;
    };

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

        // Long press (3 seconds) to enter WiFi configuration mode
        boot_button_.OnLongPress([this]() {
            power_save_timer_->WakeUp();
            ESP_LOGI(TAG, "Boot button long pressed (3s), entering WiFi configuration mode");
            GetDisplay()->ShowNotification("Đang vào chế độ cấu hình WiFi...");
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

    void InitializeIRReceiver() {
        ir_receiver_ = new IRReceiver(IR_RECEIVER_GPIO);
        ir_learn_semaphore_ = xSemaphoreCreateBinary();
        if (ir_learn_semaphore_ == nullptr) {
            ESP_LOGE(TAG, "Failed to create IR learn semaphore");
            // Continue without semaphore - learning will fail gracefully
        }
        
        // Create queue to defer callback from ISR to task context
        ir_command_queue_ = xQueueCreate(5, sizeof(IRCommandEvent*));
        if (ir_command_queue_ == nullptr) {
            ESP_LOGE(TAG, "Failed to create IR command queue");
        }
        
        // Set callback for decoded commands (only for learning mode via MCP tool)
        // This callback may be called from ISR or task context, so we defer processing to task context
        ir_receiver_->OnCommandReceived([this](uint64_t command, const std::string& protocol) {
            if (ir_learning_active_ && ir_command_queue_ != nullptr) {
                // Allocate event on heap to pass through queue
                IRCommandEvent* event = new IRCommandEvent();
                event->command = command;
                event->protocol = protocol;
                event->raw_data = ir_receiver_->GetRawData();
                
                // Try to send to queue (works from both ISR and task context)
                // First try FromISR (safe even if not in ISR)
                BaseType_t xHigherPriorityTaskWoken = pdFALSE;
                BaseType_t result = xQueueSendFromISR(ir_command_queue_, &event, &xHigherPriorityTaskWoken);
                
                if (result != pdTRUE) {
                    // If FromISR failed, try normal send (we might not be in ISR)
                    if (xQueueSend(ir_command_queue_, &event, 0) != pdTRUE) {
                        ESP_LOGW(TAG, "IR command queue full, dropping command");
                        delete event;
                    }
                } else {
                    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
                }
            }
        });
        
        // Create task to process IR commands from queue
        xTaskCreate([](void* param) {
            XINGZHI_CUBE_1_54TFT_WIFI* board = static_cast<XINGZHI_CUBE_1_54TFT_WIFI*>(param);
            IRCommandEvent* event = nullptr;
            
            while (true) {
                if (xQueueReceive(board->ir_command_queue_, &event, portMAX_DELAY) == pdTRUE) {
                    if (event && board->ir_learning_active_) {
                        // Process in task context (safe for display and semaphore operations)
                        board->learned_command_ = event->command;
                        board->learned_protocol_ = event->protocol;
                        board->learned_raw_data_ = event->raw_data;
                        board->ir_learning_active_ = false;
                        
                        if (board->ir_learn_semaphore_ != nullptr) {
                            xSemaphoreGive(board->ir_learn_semaphore_);
                        }
                        
                        std::string message = "Đã học: " + event->protocol + " 0x";
                        char hex_str[19];
                        snprintf(hex_str, sizeof(hex_str), "%016" PRIX64, event->command);
                        message += hex_str;
                        board->GetDisplay()->ShowNotification(message);
                        ESP_LOGI(TAG, "IR Command learned: %s - 0x%016" PRIX64, event->protocol.c_str(), event->command);
                    }
                    
                    if (event) {
                        delete event;
                    }
                }
            }
        }, "ir_learn_task", 4096, this, 5, &ir_learn_task_handle_);
        
        ESP_LOGI(TAG, "IR Receiver initialized on GPIO %d", IR_RECEIVER_GPIO);
    }
    
    void InitializeTools() {
        auto& mcp_server = McpServer::GetInstance();
        
        // Add IR learning tool
        mcp_server.AddTool("self.ir.learn_command",
            "Learn an IR command from the IR remote. This tool will enable the IR receiver and wait for a signal from the remote control.\n"
            "The tool will return the learned command information including protocol, command code, and raw timing data.\n"
            "Args:\n"
            "  `timeout`: Optional timeout in seconds (default: 10 seconds). If no signal is received within this time, the tool will return an error.\n"
            "Return:\n"
            "  A JSON object containing:\n"
            "    - `protocol`: The detected IR protocol (NEC, RC5, Sony, or Raw)\n"
            "    - `command`: The command code in hexadecimal format (0xXXXXXXXX)\n"
            "    - `raw_data`: Array of timing pulses in microseconds (only for Raw protocol)\n"
            "    - `success`: true if a command was learned successfully",
            PropertyList({
                Property("timeout", kPropertyTypeInteger, 10, 1, 60)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                int timeout_seconds = properties["timeout"].value<int>();
                
                // Check if IR receiver is available
                if (ir_receiver_ == nullptr) {
                    throw std::runtime_error("IR Receiver not initialized");
                }
                
                // Reset learning state
                learned_command_ = 0;
                learned_protocol_ = "";
                learned_raw_data_.clear();
                ir_learning_active_ = true;
                
                // Enable IR receiver if not already running
                if (!ir_receiver_->IsRunning()) {
                    ir_receiver_->Start();
                    ESP_LOGI(TAG, "IR Receiver started for learning");
                }
                
                // Check if semaphore is available
                if (ir_learn_semaphore_ == nullptr) {
                    ESP_LOGE(TAG, "IR learn semaphore not available");
                    cJSON* json = cJSON_CreateObject();
                    cJSON_AddBoolToObject(json, "success", false);
                    cJSON_AddStringToObject(json, "error", "IR learning system not properly initialized");
                    return json;
                }
                
                GetDisplay()->ShowNotification("Đang chờ lệnh IR...");
                ESP_LOGI(TAG, "Waiting for IR command (timeout: %d seconds)", timeout_seconds);
                
                // Wait for IR command with timeout
                // Use smaller timeout chunks to avoid watchdog issues
                TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_seconds * 1000);
                TickType_t start_time = xTaskGetTickCount();
                BaseType_t result = pdFALSE;
                
                while ((xTaskGetTickCount() - start_time) < timeout_ticks) {
                    // Check every 1 second to avoid blocking too long
                    TickType_t remaining = timeout_ticks - (xTaskGetTickCount() - start_time);
                    TickType_t wait_time = remaining > pdMS_TO_TICKS(1000) ? pdMS_TO_TICKS(1000) : remaining;
                    
                    result = xSemaphoreTake(ir_learn_semaphore_, wait_time);
                    if (result == pdTRUE) {
                        break;
                    }
                }
                
                if (result == pdTRUE) {
                    // Successfully received a command
                    cJSON* json = cJSON_CreateObject();
                    cJSON_AddBoolToObject(json, "success", true);
                    cJSON_AddStringToObject(json, "protocol", learned_protocol_.c_str());
                    
                    if (learned_protocol_ != "UNKNOWN" && learned_protocol_ != "Raw" && learned_command_ != 0) {
                        char hex_str[19];
                        snprintf(hex_str, sizeof(hex_str), "0x%016" PRIX64, learned_command_);
                        cJSON_AddStringToObject(json, "command", hex_str);
                    }
                    
                    // Always include raw data if available
                    if (!learned_raw_data_.empty()) {
                        cJSON* raw_array = cJSON_CreateArray();
                        for (size_t i = 0; i < learned_raw_data_.size() && i < 200; i++) { // Limit to 200 pulses
                            cJSON_AddItemToArray(raw_array, cJSON_CreateNumber(learned_raw_data_[i]));
                        }
                        cJSON_AddItemToObject(json, "raw_data", raw_array);
                    }
                    
                    return json;
                } else {
                    // Timeout
                    ir_learning_active_ = false;
                    GetDisplay()->ShowNotification("Hết thời gian chờ");
                    ESP_LOGW(TAG, "IR learning timeout after %d seconds", timeout_seconds);
                    
                    cJSON* json = cJSON_CreateObject();
                    cJSON_AddBoolToObject(json, "success", false);
                    cJSON_AddStringToObject(json, "error", "Timeout: No IR signal received");
                    return json;
                }
            });
        
        ESP_LOGI(TAG, "IR learning tool registered");
    }

public:
    XINGZHI_CUBE_1_54TFT_WIFI() :
        boot_button_(BOOT_BUTTON_GPIO, false, 3000, 0, false),  // long_press_time = 3000ms (3 seconds)
        volume_up_button_(VOLUME_UP_BUTTON_GPIO),
        volume_down_button_(VOLUME_DOWN_BUTTON_GPIO) {
        InitializePowerManager();
        InitializePowerSaveTimer();
        InitializeSpi();
        InitializeButtons();
        InitializeSt7789Display();
        InitializeIRReceiver();
        InitializeTools();
        GetBacklight()->RestoreBrightness();
    }

    ~XINGZHI_CUBE_1_54TFT_WIFI() {
        // Stop IR receiver if running
        if (ir_receiver_ != nullptr) {
            ir_receiver_->Stop();
            delete ir_receiver_;
            ir_receiver_ = nullptr;
        }

        // Delete task first (it uses the queue)
        if (ir_learn_task_handle_ != nullptr) {
            vTaskDelete(ir_learn_task_handle_);
            ir_learn_task_handle_ = nullptr;
        }

        // Delete semaphore
        if (ir_learn_semaphore_ != nullptr) {
            vSemaphoreDelete(ir_learn_semaphore_);
            ir_learn_semaphore_ = nullptr;
        }
        
        // Delete queue
        if (ir_command_queue_ != nullptr) {
            vQueueDelete(ir_command_queue_);
            ir_command_queue_ = nullptr;
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
