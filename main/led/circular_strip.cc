#include "circular_strip.h"
#include "application.h"
#include <esp_log.h>
#include <memory>

#define TAG "CircularStrip"

#define BLINK_INFINITE -1

CircularStrip::CircularStrip(gpio_num_t gpio, uint8_t max_leds) : gpio_num_(gpio), max_leds_(max_leds) {
    // If the gpio is not connected, you should use NoLed class
    assert(gpio != GPIO_NUM_NC);

    colors_.resize(max_leds_);

    led_strip_config_t strip_config = {};
    strip_config.strip_gpio_num = gpio;
    strip_config.max_leds = max_leds_;
    strip_config.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
    strip_config.led_model = LED_MODEL_WS2812;

    led_strip_rmt_config_t rmt_config = {};
    rmt_config.resolution_hz = 10 * 1000 * 1000; // 10MHz

    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip_));
    led_strip_clear(led_strip_);

    esp_timer_create_args_t strip_timer_args = {
        .callback = [](void *arg) {
            auto strip = static_cast<CircularStrip*>(arg);
            std::lock_guard<std::mutex> lock(strip->mutex_);
            if (strip->strip_callback_ != nullptr) {
                strip->strip_callback_();
            }
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "strip_timer",
        .skip_unhandled_events = false,
    };
    ESP_ERROR_CHECK(esp_timer_create(&strip_timer_args, &strip_timer_));
}

CircularStrip::~CircularStrip() {
    esp_timer_stop(strip_timer_);
    if (led_strip_ != nullptr) {
        led_strip_del(led_strip_);
    }
}


void CircularStrip::SetAllColor(StripColor color) {
    std::lock_guard<std::mutex> lock(mutex_);
    esp_timer_stop(strip_timer_);
    for (int i = 0; i < max_leds_; i++) {
        colors_[i] = color;
        led_strip_set_pixel(led_strip_, i, color.red, color.green, color.blue);
    }
    led_strip_refresh(led_strip_);
}

void CircularStrip::SetSingleColor(uint8_t index, StripColor color) {
    std::lock_guard<std::mutex> lock(mutex_);
    esp_timer_stop(strip_timer_);
    colors_[index] = color;
    led_strip_set_pixel(led_strip_, index, color.red, color.green, color.blue);
    led_strip_refresh(led_strip_);
}

void CircularStrip::Blink(StripColor color, int interval_ms) {
    for (int i = 0; i < max_leds_; i++) {
        colors_[i] = color;
    }
    StartStripTask(interval_ms, [this]() {
        static bool on = true;
        if (on) {
            for (int i = 0; i < max_leds_; i++) {
                led_strip_set_pixel(led_strip_, i, colors_[i].red, colors_[i].green, colors_[i].blue);
            }
            led_strip_refresh(led_strip_);
        } else {
            led_strip_clear(led_strip_);
        }
        on = !on;
    });
}

void CircularStrip::FadeOut(int interval_ms) {
    StartStripTask(interval_ms, [this]() {
        bool all_off = true;
        for (int i = 0; i < max_leds_; i++) {
            colors_[i].red /= 2;
            colors_[i].green /= 2;
            colors_[i].blue /= 2;
            if (colors_[i].red != 0 || colors_[i].green != 0 || colors_[i].blue != 0) {
                all_off = false;
            }
            led_strip_set_pixel(led_strip_, i, colors_[i].red, colors_[i].green, colors_[i].blue);
        }
        if (all_off) {
            led_strip_clear(led_strip_);
            esp_timer_stop(strip_timer_);
        } else {
            led_strip_refresh(led_strip_);
        }
    });
}

void CircularStrip::Breathe(StripColor low, StripColor high, int interval_ms) {
    // Use shared_ptr to avoid memory leaks and state pollution
    // These will be automatically cleaned up when the lambda is destroyed
    std::shared_ptr<int> direction = std::make_shared<int>(1);
    std::shared_ptr<int> brightness = std::make_shared<int>(low_brightness_);
    
    StartStripTask(interval_ms, [this, low, high, direction, brightness]() {
        // Note: mutex is already held by the timer callback wrapper (line 30)
        // No need to lock again here to avoid deadlock
        if (led_strip_ == nullptr) {
            return;  // LED strip disabled, shared_ptr will auto-cleanup
        }
        
        *brightness += *direction * 2;
        if (*brightness >= default_brightness_) {
            *brightness = default_brightness_;
            *direction = -1;
        } else if (*brightness <= low_brightness_) {
            *brightness = low_brightness_;
            *direction = 1;
        }
        
        for (int i = 0; i < max_leds_; i++) {
            uint8_t r = (high.red * (*brightness)) / default_brightness_;
            uint8_t g = (high.green * (*brightness)) / default_brightness_;
            uint8_t b = (high.blue * (*brightness)) / default_brightness_;
            led_strip_set_pixel(led_strip_, i, r, g, b);
        }
        led_strip_refresh(led_strip_);
    });
}

void CircularStrip::Scroll(StripColor low, StripColor high, int length, int interval_ms) {
    // Validate length to prevent division by zero
    if (length <= 0) {
        ESP_LOGW(TAG, "Scroll called with invalid length (%d), using default length 1", length);
        length = 1;
    }
    
    // Use shared_ptr to avoid memory leaks and state pollution
    std::shared_ptr<int> position = std::make_shared<int>(0);
    
    StartStripTask(interval_ms, [this, low, high, length, position]() {
        // Note: mutex is already held by the timer callback wrapper (line 30)
        // No need to lock again here to avoid deadlock
        if (led_strip_ == nullptr) {
            return;  // LED strip disabled, shared_ptr will auto-cleanup
        }
        
        for (int i = 0; i < max_leds_; i++) {
            int distance = abs(i - *position);
            if (distance > length) {
                distance = length;
            }
            
            // length is guaranteed to be > 0 due to validation above
            uint8_t r = low.red + ((high.red - low.red) * (length - distance)) / length;
            uint8_t g = low.green + ((high.green - low.green) * (length - distance)) / length;
            uint8_t b = low.blue + ((high.blue - low.blue) * (length - distance)) / length;
            
            led_strip_set_pixel(led_strip_, i, r, g, b);
        }
        led_strip_refresh(led_strip_);
        
        (*position)++;
        if (*position >= max_leds_ + length) {
            *position = 0;
        }
    });
}

void CircularStrip::SetBrightness(uint8_t default_brightness, uint8_t low_brightness) {
    default_brightness_ = default_brightness;
    low_brightness_ = low_brightness;
}

void CircularStrip::StartStripTask(int interval_ms, std::function<void()> cb) {
    if (led_strip_ == nullptr) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    esp_timer_stop(strip_timer_);
    strip_callback_ = cb;
    esp_timer_start_periodic(strip_timer_, interval_ms * 1000);
}

void CircularStrip::Rainbow(StripColor low, StripColor high, int interval_ms) {
    StartStripTask(interval_ms, [this, low, high]() {
        static int hue = 0;
        
        for (int i = 0; i < max_leds_; i++) {
            int led_hue = (hue + (i * 360 / max_leds_)) % 360;
            
            uint8_t r, g, b;
            if (led_hue < 60) {
                r = 255;
                g = (led_hue * 255) / 60;
                b = 0;
            } else if (led_hue < 120) {
                r = ((120 - led_hue) * 255) / 60;
                g = 255;
                b = 0;
            } else if (led_hue < 180) {
                r = 0;
                g = 255;
                b = ((led_hue - 120) * 255) / 60;
            } else if (led_hue < 240) {
                r = 0;
                g = ((240 - led_hue) * 255) / 60;
                b = 255;
            } else if (led_hue < 300) {
                r = ((led_hue - 240) * 255) / 60;
                g = 0;
                b = 255;
            } else {
                r = 255;
                g = 0;
                b = ((360 - led_hue) * 255) / 60;
            }
            
            led_strip_set_pixel(led_strip_, i, r, g, b);
        }
        led_strip_refresh(led_strip_);
        
        hue = (hue + 5) % 360;
    });
}

void CircularStrip::OnStateChanged() {
    auto& app = Application::GetInstance();
    auto device_state = app.GetDeviceState();
    switch (device_state) {
        case kDeviceStateStarting: {
            StripColor color = { low_brightness_, low_brightness_, default_brightness_ };
            Blink(color, 100);
            break;
        }
        case kDeviceStateWifiConfiguring: {
            StripColor color = { low_brightness_, low_brightness_, default_brightness_ };
            Blink(color, 500);
            break;
        }
        case kDeviceStateIdle: {
            StripColor color = { 0, 0, 0 };
            SetAllColor(color);
            break;
        }
        case kDeviceStateConnecting: {
            StripColor color = { low_brightness_, low_brightness_, default_brightness_ };
            SetAllColor(color);
            break;
        }
        case kDeviceStateListening:
        case kDeviceStateAudioTesting: {
            if (app.IsVoiceDetected()) {
                StripColor color = { default_brightness_, low_brightness_, low_brightness_ };
                SetAllColor(color);
            } else {
                StripColor color = { low_brightness_, low_brightness_, low_brightness_ };
                SetAllColor(color);
            }
            break;
        }
        case kDeviceStateSpeaking: {
            StripColor color = { low_brightness_, default_brightness_, low_brightness_ };
            SetAllColor(color);
            break;
        }
        case kDeviceStateUpgrading: {
            StripColor color = { low_brightness_, default_brightness_, low_brightness_ };
            Blink(color, 100);
            break;
        }
        case kDeviceStateActivating: {
            StripColor color = { low_brightness_, default_brightness_, low_brightness_ };
            Blink(color, 500);
            break;
        }
        default:
            ESP_LOGW(TAG, "Unknown led strip event: %d", device_state);
            return;
    }
}

void CircularStrip::Disable() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (led_strip_ != nullptr) {
        esp_timer_stop(strip_timer_);
        led_strip_del(led_strip_);
        led_strip_ = nullptr;
        ESP_LOGI(TAG, "LED strip disabled (RMT channel freed)");
    }
}

void CircularStrip::Enable() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (led_strip_ == nullptr) {
        led_strip_config_t strip_config = {};
        strip_config.strip_gpio_num = gpio_num_;
        strip_config.max_leds = max_leds_;
        strip_config.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
        strip_config.led_model = LED_MODEL_WS2812;

        led_strip_rmt_config_t rmt_config = {};
        rmt_config.resolution_hz = 10 * 1000 * 1000; // 10MHz

        esp_err_t ret = led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip_);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to recreate LED strip: %s", esp_err_to_name(ret));
            return;
        }
        led_strip_clear(led_strip_);
        
        // Restore colors if set
        for (int i = 0; i < max_leds_; i++) {
            if (colors_[i].red != 0 || colors_[i].green != 0 || colors_[i].blue != 0) {
                led_strip_set_pixel(led_strip_, i, colors_[i].red, colors_[i].green, colors_[i].blue);
            }
        }
        led_strip_refresh(led_strip_);
        
        ESP_LOGI(TAG, "LED strip re-enabled");
    }
}
