/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <cstdint>
#include <cstddef>
#include <M5Unified.hpp>
#include <M5GFX.h>
#include <M5PM1.h>
#include <driver/gpio.h>
#include "wifi/hal_wifi.h"

/**
 * @brief Identifiers for persisted settings values.
 */
enum SettingKey : uint8_t {
    SETTING_WIFI_SSID,
    SETTING_WIFI_PASSWORD,
    SETTING_ROTATION,
    SETTING_AUTO_SLIDESHOW,
    SETTING_INTERVAL,
    SETTING_CURRENT_MODE,
    SETTING_BOOT_SOUND,
    SETTING_DEVICE_NAME,
    SETTING_LOW_POWER_MODE,
    SETTING_GATEWAY_URL,
    SETTING_GATEWAY_TOKEN,
    SETTING_GATEWAY_ENABLED,
    SETTING_GATEWAY_POLL_MINUTES
};

/** @brief Local slideshow mode identifier. */
static constexpr const char* MODE_ID_LOCAL = "mode_1";

/**
 * @brief Application mode values used by the HAL.
 */
enum AppMode : uint8_t { APP_MODE_NONE = 0, APP_MODE_LOCAL };

/** @brief Normalizes a mode identifier. */
bool normalize_mode_id(const char* input, char* output, size_t output_size);
/** @brief Normalizes a device name. */
bool normalize_device_name(const char* input, char* output, size_t output_size);
/** @brief Checks whether a mode identifier is supported. */
bool is_supported_mode_id(const char* mode_id);
/** @brief Converts a mode identifier to AppMode. */
AppMode app_mode_from_mode_id(const char* mode_id);
/** @brief Converts AppMode to its mode identifier. */
const char* mode_id_from_app_mode(AppMode mode);
/** @brief Compares two C strings. */
int cstring_compare(const char* lhs, const char* rhs);
/** @brief Copies a C string into a fixed-size buffer. */
size_t cstring_copy(char* dst, const char* src, size_t dst_size);

/**
 * @brief Persisted device settings.
 */
struct Settings {
    char wifi_ssid[64];
    char wifi_password[64];
    uint8_t rotation;  // 0=landscape, 1=portrait
    bool auto_slideshow;
    int interval_minutes;
    char current_mode[16];  // "" (none, factory default) / "mode_1" (local)
    bool boot_sound;
    char device_name[64];
    bool low_power_mode;
    char gateway_url[192];
    char gateway_token[96];
    bool gateway_enabled;
    uint16_t gateway_poll_minutes;
};

/** @brief Summary statistics for a short microphone capture. */
struct MicrophoneDiagnostics {
    uint32_t sample_count = 0;
    int32_t dc_offset     = 0;
    uint32_t peak         = 0;
    uint32_t rms          = 0;
};

#define OPERATION_EVENT_FAILED           BIT0
#define OPERATION_EVENT_ERROR_IMAGE_READ BIT1
#define OPERATION_EVENT_REFRESH_START    BIT2
#define OPERATION_EVENT_REFRESH_COMPLETE BIT3
#define OPERATION_EVENT_SUCCESS          BIT4
#define OPERATION_EVENT_WAITING_WIFI     BIT5
#define OPERATION_EVENT_STARTUP_SUCCESS  BIT6

/**
 * @brief Central hardware abstraction for the device.
 */
class Hal {
public:
    M5PM1 pm1;
    M5Canvas* Canvas    = nullptr;
    float temperature   = 0.0f;
    float humidity      = 0.0f;
    bool battery_ok     = false;
    bool vin_ok         = false;
    bool sd_inserted    = false;
    uint16_t battery_mv = 0;
    uint16_t vin_mv     = 0;

    Settings settings;

    /** @brief Initializes hardware and runtime state. */
    void init();
    /** @brief Loads settings from storage. */
    void settingsInit();
    /** @brief Saves one settings field identified by key. */
    void settingsSave(SettingKey key);
    /** @brief Locks settings access. */
    void settingsLock();
    /** @brief Unlocks settings access. */
    void settingsUnlock();
    /** @brief Runs the periodic hardware update loop. */
    void update();
    /** @brief Reads the device MAC address. */
    void getDeviceMac(uint8_t* mac);
    /** @brief Returns whether an SD card is inserted. */
    bool isSDCardInserted();
    /** @brief Reads temperature and humidity from SHT40. */
    bool sht40Read(float* temp, float* humi);
    /** @brief Returns the latest valid SHT40 sample without accessing I2C. */
    bool sht40CachedRead(float* temp, float* humi);
    /** @brief Captures a short microphone sample and returns level statistics. */
    bool microphoneDiagnostics(MicrophoneDiagnostics* result);
    /** @brief Runs a visible sequence across the onboard RGB LEDs. */
    size_t rgbLedSelfTest();
    /** @brief Plays a short two-note speaker test. */
    bool speakerSelfTest();
    /** @brief Writes a byte to RX8130 RAM. */
    bool rx8130RamWrite(uint8_t index, uint8_t value);
    /** @brief Reads a byte from RX8130 RAM. */
    bool rx8130RamRead(uint8_t index, uint8_t* value);
    /** @brief Returns whether low power mode is enabled. */
    bool lowPowerModeEnabled() const;
    /** @brief Returns whether the current boot was triggered by RTC wake. */
    bool isRtcWakeBoot() const;
    /** @brief Detects the current wake source. */
    void detectWakeSource();
    /** @brief Configures the RTC wake pin. */
    bool configureRtcWakePin();
    /** @brief Schedules the next RTC wake by minutes. */
    bool scheduleNextWakeMinutes(int interval_minutes);
    /** @brief Returns the external RTC as a UTC epoch timestamp. */
    int64_t rtcEpochUtc();
    /** @brief Sets the external RTC from a UTC epoch timestamp. */
    bool setRtcEpochUtc(int64_t epoch_seconds);
    /** @brief Schedules an absolute external RTC wake using UTC epoch seconds. */
    bool scheduleWakeEpochUtc(int64_t epoch_seconds);
    /** @brief Returns whether USB/external input power is present. */
    bool externalPowerPresent();
    /** @brief Clears wake-related flags. */
    void clearWakeFlags();
    /** @brief Requests PMIC power-off and reports whether the command was accepted. */
    bool powerOff();
    /** @brief Sends a status event. */
    bool statusEventSend(EventBits_t event_bits);

private:
    void i2cBusRecovery();
    static void LedStatusIndicateTask(void* task_parameters);
    static constexpr m5pm1_gpio_num_t SD_DEC            = M5PM1_GPIO_NUM_1;
    static constexpr m5pm1_gpio_num_t EPD_EN            = M5PM1_GPIO_NUM_0;
    static constexpr m5pm1_gpio_num_t SD_DET_EN         = M5PM1_GPIO_NUM_4;
    static constexpr gpio_num_t SYS_SCL_PIN             = GPIO_NUM_2;
    static constexpr gpio_num_t SYS_SDA_PIN             = GPIO_NUM_3;
    TaskHandle_t _led_status_indicate_task_handle       = nullptr;
    EventGroupHandle_t _led_status_indicate_event_group = nullptr;
    SemaphoreHandle_t _settings_mutex                   = nullptr;
    SemaphoreHandle_t _sht40_cache_mutex                = nullptr;
    bool _sht40_cache_valid                             = false;
    uint32_t _sht40_last_attempt_ms                     = 0;

    static constexpr uint8_t SHT4X_ADDR             = 0x44;
    static constexpr uint8_t SHT4X_CMD_HI_PRE       = 0xFD;
    static constexpr uint8_t RX8130_ADDR            = 0x32;
    static constexpr uint8_t RX8130_RAM_BASE        = 0x20;  // RAM start register address
    static constexpr uint8_t RX8130_RAM_SIZE        = 4;     // 4 bytes (0x20~0x23)
    static constexpr m5pm1_gpio_num_t RTC_WAKE_GPIO = M5PM1_GPIO_NUM_2;
    bool _is_rtc_wake_boot                          = false;

    typedef enum {
        LED_STATE_IDLE,
        LED_STATE_SUCCESS_FADE,
        LED_STATE_ERROR_FADE,
        LED_STATE_REFRESH_START_BLINK,
        LED_STATE_REFRESH_COMPLETE_BLINK,
        LED_STATE_WAITING_WIFI_BLINK,
        LED_STATE_SUCCESS_STEADY_BLINK,
        LED_STATE_REFRESH_START_STEADY_BLINK,
        LED_STATE_REFRESH_COMPLETE_STEADY_BLINK
    } LedState;
};

extern Hal hal;
