#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "display/lcd_display.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "power_save_timer.h"
#include "led/single_led.h"
#include "led/circular_strip.h"
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
#include <atomic>

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
    std::atomic<bool> ir_learning_active_{false};  // Atomic to prevent data race between ISR and task context
    std::atomic<bool> shutting_down_{false};  // Flag to signal task that board is being destroyed
    uint64_t learned_command_ = 0;
    std::string learned_protocol_ = "";
    std::vector<uint32_t> learned_raw_data_;
    
    // Event structure for passing IR command data through queue
    // Using fixed-size arrays to avoid memory allocation in ISR context
    // Raw data is fetched in task context, not copied in ISR
    struct IRCommandEvent {
        uint64_t command;
        char protocol[32];  // Fixed-size buffer for protocol name (max 31 chars + null terminator)
        // Note: raw_data is fetched in task context via ir_receiver_->GetRawData()
        // to avoid vector copy operations in ISR context
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
            GetDisplay()->ShowNotification(Lang::Strings::ENTERING_WIFI_CONFIG_MODE);
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
        // Queue stores IRCommandEvent directly (not pointers) to avoid allocation in ISR
        ir_command_queue_ = xQueueCreate(5, sizeof(IRCommandEvent));
        if (ir_command_queue_ == nullptr) {
            ESP_LOGE(TAG, "Failed to create IR command queue");
            // Cannot proceed without queue - IR learning will not work
            return;
        }
        
        // Set callback for decoded commands (only for learning mode via MCP tool)
        // This callback may be called from ISR or task context, so we defer processing to task context
        // IMPORTANT: No memory allocation in this callback - use fixed-size structure
        ir_receiver_->OnCommandReceived([this](uint64_t command, const std::string& protocol) {
            if (ir_learning_active_.load(std::memory_order_acquire) && ir_command_queue_ != nullptr) {
                // Use stack-allocated event (no heap allocation in ISR context)
                IRCommandEvent event = {};
                event.command = command;
                
                // Copy protocol string safely using memcpy (no string allocation in ISR)
                // Truncate if too long to fit in fixed buffer
                size_t protocol_len = protocol.length();
                if (protocol_len >= sizeof(event.protocol)) {
                    protocol_len = sizeof(event.protocol) - 1;
                }
                memcpy(event.protocol, protocol.c_str(), protocol_len);
                event.protocol[protocol_len] = '\0';
                
                // Note: raw_data will be fetched in task context via GetRawData()
                // to avoid vector copy operations in ISR context
                
                // Send to queue - this callback is called from RMT ISR context
                // Must use ISR-safe function only - never use xQueueSend from ISR
                BaseType_t xHigherPriorityTaskWoken = pdFALSE;
                BaseType_t result = xQueueSendFromISR(ir_command_queue_, &event, &xHigherPriorityTaskWoken);
                
                if (result == pdTRUE) {
                    // Successfully queued - yield if higher priority task was woken
                    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
                } else {
                    // Queue full - drop the event (better than blocking in ISR)
                    // This should be rare if queue size is appropriate
                    ESP_LOGW(TAG, "IR command queue full, dropping command");
                }
            }
        });
        
        // Create task to process IR commands from queue
        // Only create task if queue was successfully created
        BaseType_t task_result = xTaskCreate([](void* param) {
            XINGZHI_CUBE_1_54TFT_WIFI* board = static_cast<XINGZHI_CUBE_1_54TFT_WIFI*>(param);
            IRCommandEvent event;
            
            while (true) {
                // Check if board is being destroyed - exit immediately if so
                if (board->shutting_down_.load(std::memory_order_acquire)) {
                    ESP_LOGI(TAG, "IR learn task exiting due to shutdown flag");
                    break;
                }
                
                // Defensive check: ensure queue is valid before using it
                if (board->ir_command_queue_ == nullptr) {
                    ESP_LOGE(TAG, "IR command queue is null in task, exiting");
                    break;
                }
                
                BaseType_t result = xQueueReceive(board->ir_command_queue_, &event, portMAX_DELAY);
                
                // If queue was deleted while waiting, xQueueReceive returns pdFALSE
                // This allows the task to exit gracefully when the destructor deletes the queue
                if (result != pdTRUE) {
                    break;  // Queue deleted or invalid, exit task gracefully
                }
                
                // Check shutdown flag again after receiving event (destructor may have run)
                if (board->shutting_down_.load(std::memory_order_acquire)) {
                    ESP_LOGI(TAG, "IR learn task exiting due to shutdown flag after event");
                    break;
                }
                
                if (board->ir_learning_active_.load(std::memory_order_acquire)) {
                    // Process in task context (safe for display, semaphore, and memory operations)
                    // Copy event data to local variables first to minimize access to board members
                    uint64_t command = event.command;
                    std::string protocol(event.protocol);
                    
                    // Fetch raw data now (in task context, safe to copy vector)
                    // Check if ir_receiver_ is still valid (may be deleted by destructor)
                    std::vector<uint32_t> raw_data;
                    if (board->ir_receiver_ != nullptr && !board->shutting_down_.load(std::memory_order_acquire)) {
                        raw_data = board->ir_receiver_->GetRawData();
                    }
                    
                    // Only update board state if not shutting down
                    if (!board->shutting_down_.load(std::memory_order_acquire)) {
                        board->learned_command_ = command;
                        board->learned_protocol_ = protocol;
                        board->learned_raw_data_ = raw_data;
                        board->ir_learning_active_.store(false, std::memory_order_release);
                        
                        if (board->ir_learn_semaphore_ != nullptr) {
                            xSemaphoreGive(board->ir_learn_semaphore_);
                        }
                        
                        // Only show notification if receiver is still valid and not shutting down
                        // Cache pointers to avoid TOCTOU race condition
                        IRReceiver* ir_receiver = board->ir_receiver_;
                        if (ir_receiver != nullptr && !board->shutting_down_.load(std::memory_order_acquire)) {
                            // Cache display pointer to avoid TOCTOU
                            Display* display = board->GetDisplay();
                            if (display != nullptr && !board->shutting_down_.load(std::memory_order_acquire)) {
                                std::string message = std::string(Lang::Strings::IR_LEARNED) + protocol + " 0x";
                                char hex_str[19];
                                snprintf(hex_str, sizeof(hex_str), "%016" PRIX64, command);
                                message += hex_str;
                                display->ShowNotification(message);
                                ESP_LOGI(TAG, "IR Command learned: %s - 0x%016" PRIX64, protocol.c_str(), command);
                            }
                        }
                    }
                }
            }
            
            // Task exits gracefully when queue is deleted or shutdown flag is set
            ESP_LOGI(TAG, "IR learn task exiting");
            vTaskDelete(nullptr);  // Delete self
        }, "ir_learn_task", 4096, this, 5, &ir_learn_task_handle_);
        
        if (task_result != pdPASS) {
            ESP_LOGE(TAG, "Failed to create IR learning task");
            // Clean up queue if task creation failed
            vQueueDelete(ir_command_queue_);
            ir_command_queue_ = nullptr;
            return;
        }
        
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
                ir_learning_active_.store(true, std::memory_order_release);
                
                // Temporarily disable LED strips to free RMT channels for IR learning
                // LED uses RMT TX channels which share resources with RMT RX
                Led* led = GetLed();
                bool led_was_enabled = false;
                
                // Disable LED strip if it's a SingleLed or CircularStrip to free RMT channels
                if (led != nullptr) {
                    // Try to cast to SingleLed or CircularStrip and disable
                    SingleLed* single_led = dynamic_cast<SingleLed*>(led);
                    CircularStrip* circular_strip = dynamic_cast<CircularStrip*>(led);
                    
                    if (single_led != nullptr) {
                        ESP_LOGI(TAG, "Disabling SingleLed to free RMT channels...");
                        single_led->Disable();
                        led_was_enabled = true;
                        ESP_LOGI(TAG, "SingleLed disabled successfully");
                    } else if (circular_strip != nullptr) {
                        ESP_LOGI(TAG, "Disabling CircularStrip to free RMT channels...");
                        circular_strip->Disable();
                        led_was_enabled = true;
                        ESP_LOGI(TAG, "CircularStrip disabled successfully");
                    } else {
                        ESP_LOGI(TAG, "LED is not a SingleLed or CircularStrip, skipping disable");
                    }
                } else {
                    ESP_LOGI(TAG, "No LED found, skipping LED disable");
                }
                
                // Longer delay to allow RMT channels to be fully released after LED deletion
                // RMT driver needs time to clean up internal state
                ESP_LOGI(TAG, "Waiting for RMT channels to be released...");
                vTaskDelay(pdMS_TO_TICKS(300));  // Increased delay for RMT cleanup
                
                // Ensure IR receiver is stopped and cleaned up before starting
                // This helps free any RMT channels it might be holding
                ir_receiver_->Stop();
                vTaskDelay(pdMS_TO_TICKS(100));  // Allow time for RMT channel release
                
                // Enable IR receiver - reuse existing channel if available
                // Start() will handle channel reuse or creation
                ir_receiver_->Start();
                
                // Retry with delay if first attempt fails (allows time for other RMT channels to release)
                if (!ir_receiver_->IsRunning()) {
                    ESP_LOGW(TAG, "First IR receiver start attempt failed, retrying after delay...");
                    vTaskDelay(pdMS_TO_TICKS(200));  // Wait longer for RMT channels to be released
                    ir_receiver_->Start();
                }
                
                if (!ir_receiver_->IsRunning()) {
                    ESP_LOGE(TAG, "Failed to start IR Receiver after retry");
                    ir_learning_active_.store(false, std::memory_order_release);
                    
                    // Re-enable LED before returning on error
                    if (led_was_enabled) {
                        Led* led = GetLed();
                        if (led != nullptr) {
                            SingleLed* single_led = dynamic_cast<SingleLed*>(led);
                            CircularStrip* circular_strip = dynamic_cast<CircularStrip*>(led);
                            
                            if (single_led != nullptr) {
                                ESP_LOGI(TAG, "Re-enabling SingleLed after IR receiver start failure...");
                                single_led->Enable();
                                ESP_LOGI(TAG, "SingleLed re-enabled successfully");
                            } else if (circular_strip != nullptr) {
                                ESP_LOGI(TAG, "Re-enabling CircularStrip after IR receiver start failure...");
                                circular_strip->Enable();
                                ESP_LOGI(TAG, "CircularStrip re-enabled successfully");
                            }
                            
                            // Restore LED state
                            led->OnStateChanged();
                        }
                    }
                    
                    cJSON* json = cJSON_CreateObject();
                    cJSON_AddBoolToObject(json, "success", false);
                    cJSON_AddStringToObject(json, "error", "Failed to start IR receiver - no free RMT channels. Try disabling LED or other RMT devices.");
                    return json;
                }
                ESP_LOGI(TAG, "IR Receiver started for learning");
                
                // Check if semaphore is available
                if (ir_learn_semaphore_ == nullptr) {
                    ESP_LOGE(TAG, "IR learn semaphore not available");
                    // Stop IR receiver before returning to prevent resource leak
                    ir_receiver_->Stop();
                    ir_learning_active_.store(false, std::memory_order_release);
                    
                    // Re-enable LED before returning on error
                    if (led_was_enabled) {
                        Led* led = GetLed();
                        if (led != nullptr) {
                            SingleLed* single_led = dynamic_cast<SingleLed*>(led);
                            CircularStrip* circular_strip = dynamic_cast<CircularStrip*>(led);
                            
                            if (single_led != nullptr) {
                                ESP_LOGI(TAG, "Re-enabling SingleLed after semaphore check failure...");
                                single_led->Enable();
                                ESP_LOGI(TAG, "SingleLed re-enabled successfully");
                            } else if (circular_strip != nullptr) {
                                ESP_LOGI(TAG, "Re-enabling CircularStrip after semaphore check failure...");
                                circular_strip->Enable();
                                ESP_LOGI(TAG, "CircularStrip re-enabled successfully");
                            }
                            
                            // Restore LED state
                            led->OnStateChanged();
                        }
                    }
                    
                    cJSON* json = cJSON_CreateObject();
                    cJSON_AddBoolToObject(json, "success", false);
                    cJSON_AddStringToObject(json, "error", "IR learning system not properly initialized");
                    return json;
                }
                
                GetDisplay()->ShowNotification(Lang::Strings::IR_WAITING_FOR_COMMAND);
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
                
                // Stop IR receiver after learning attempt (success or timeout)
                // Channel is kept for reuse, not deleted
                ir_receiver_->Stop();
                
                // Re-enable LED strips after IR learning completes
                if (led_was_enabled) {
                    Led* led = GetLed();
                    if (led != nullptr) {
                        SingleLed* single_led = dynamic_cast<SingleLed*>(led);
                        CircularStrip* circular_strip = dynamic_cast<CircularStrip*>(led);
                        
                        if (single_led != nullptr) {
                            single_led->Enable();
                            ESP_LOGI(TAG, "Re-enabled SingleLed");
                        } else if (circular_strip != nullptr) {
                            circular_strip->Enable();
                            ESP_LOGI(TAG, "Re-enabled CircularStrip");
                        }
                        
                        // Restore LED state
                        led->OnStateChanged();
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
                    ir_learning_active_.store(false, std::memory_order_release);
                    GetDisplay()->ShowNotification(Lang::Strings::IR_TIMEOUT);
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
        // Step 1: Set shutdown flag to signal task to stop accessing board members
        // This must be done first to prevent use-after-free
        shutting_down_.store(true, std::memory_order_release);
        
        // Step 2: Stop IR receiver to prevent new callbacks from accessing resources
        if (ir_receiver_ != nullptr) {
            ir_receiver_->Stop();
            delete ir_receiver_;
            ir_receiver_ = nullptr;  // Set to null immediately after deletion
        }

        // Step 3: Delete queue to unblock the task waiting on xQueueReceive
        // This allows the task to exit gracefully instead of being forcefully terminated
        // Note: ir_receiver_ is already null and shutting_down_ is set, so task can safely check before accessing
        if (ir_command_queue_ != nullptr) {
            vQueueDelete(ir_command_queue_);
            ir_command_queue_ = nullptr;
        }

        // Step 4: Wait for task to exit naturally after queue deletion and shutdown flag
        // The task will see xQueueReceive fail or shutdown flag and exit its loop
        if (ir_learn_task_handle_ != nullptr) {
            // Wait for task to actually exit (poll with timeout)
            const int max_wait_ms = 500;  // Maximum wait time
            const int poll_interval_ms = 10;  // Check every 10ms
            int waited_ms = 0;
            
            while (waited_ms < max_wait_ms) {
                eTaskState task_state = eTaskGetState(ir_learn_task_handle_);
                if (task_state == eDeleted || task_state == eInvalid) {
                    break;  // Task has exited
                }
                vTaskDelay(pdMS_TO_TICKS(poll_interval_ms));
                waited_ms += poll_interval_ms;
            }
            
            // If task still exists after waiting, force delete it
            eTaskState task_state = eTaskGetState(ir_learn_task_handle_);
            if (task_state != eDeleted && task_state != eInvalid) {
                ESP_LOGW(TAG, "IR learn task did not exit naturally, forcing deletion");
                vTaskDelete(ir_learn_task_handle_);
            }
            ir_learn_task_handle_ = nullptr;
        }

        // Step 5: Delete semaphore last (no dependencies)
        if (ir_learn_semaphore_ != nullptr) {
            vSemaphoreDelete(ir_learn_semaphore_);
            ir_learn_semaphore_ = nullptr;
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
