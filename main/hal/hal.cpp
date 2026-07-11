/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"
#include "hal/power_policy.h"
#include "hal/rtc_alarm_policy.h"
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <cmath>
#include <esp_mac.h>
#include <esp_timer.h>
#include <M5Unified.hpp>
#include <M5GFX.h>
#include "storage/hal_storage.h"
#include "driver/gpio.h"
#include "esp_rom_gpio.h"
#include "esp_rom_sys.h"
#include "wifi/hal_wifi.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "freertos/semphr.h"

using namespace hal_wifi;

static const char* TAG = "Hal";

/* ---------- millis() ---------- */
static inline uint32_t millis_()
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static uint8_t sht4x_crc8(const uint8_t* data, size_t length)
{
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0x31) : static_cast<uint8_t>(crc << 1);
        }
    }
    return crc;
}

int cstring_compare(const char* lhs, const char* rhs)
{
    if (lhs == nullptr && rhs == nullptr) return 0;
    if (lhs == nullptr) return -1;
    if (rhs == nullptr) return 1;

    while (*lhs && (*lhs == *rhs)) {
        ++lhs;
        ++rhs;
    }

    return static_cast<unsigned char>(*lhs) - static_cast<unsigned char>(*rhs);
}

size_t cstring_copy(char* dst, const char* src, size_t dst_size)
{
    size_t src_len = 0;
    if (src != nullptr) {
        while (src[src_len] != '\0') {
            ++src_len;
        }
    }

    if (dst_size == 0 || dst == nullptr) {
        return src_len;
    }

    size_t copy_len = src_len;
    if (copy_len >= dst_size) {
        copy_len = dst_size - 1;
    }

    for (size_t i = 0; i < copy_len; ++i) {
        dst[i] = src[i];
    }
    dst[copy_len] = '\0';

    return src_len;
}

bool normalize_mode_id(const char* input, char* output, size_t output_size)
{
    const char* normalized = "";
    bool changed           = false;

    if (input && cstring_compare(input, MODE_ID_LOCAL) == 0) {
        normalized = MODE_ID_LOCAL;
    } else if (input && input[0]) {
        changed = true;
    }

    cstring_copy(output, normalized, output_size);
    return changed;
}

bool normalize_device_name(const char* input, char* output, size_t output_size)
{
    static constexpr const char* k_default_device_name = "papercolor";

    if (output == nullptr || output_size == 0) {
        return true;
    }

    size_t write_index = 0;
    bool changed       = false;

    if (input != nullptr) {
        while (*input != '\0' && write_index + 1 < output_size) {
            char ch = *input++;
            if (ch >= 'A' && ch <= 'Z') {
                ch      = (char)(ch - 'A' + 'a');
                changed = true;
            }

            bool is_alpha = ch >= 'a' && ch <= 'z';
            bool is_digit = ch >= '0' && ch <= '9';
            bool is_dash  = ch == '-';
            if (!(is_alpha || is_digit || is_dash)) {
                changed = true;
                continue;
            }

            if (ch == '-' && write_index == 0) {
                changed = true;
                continue;
            }

            output[write_index++] = ch;
        }

        while (write_index > 0 && output[write_index - 1] == '-') {
            --write_index;
            changed = true;
        }
    } else {
        changed = true;
    }

    output[write_index] = '\0';

    if (write_index == 0) {
        cstring_copy(output, k_default_device_name, output_size);
        return true;
    }

    if (input == nullptr) {
        return true;
    }

    return changed || cstring_compare(output, input) != 0;
}

bool is_supported_mode_id(const char* mode_id)
{
    return mode_id && cstring_compare(mode_id, MODE_ID_LOCAL) == 0;
}

AppMode app_mode_from_mode_id(const char* mode_id)
{
    if (!mode_id || !mode_id[0]) return APP_MODE_NONE;
    return cstring_compare(mode_id, MODE_ID_LOCAL) == 0 ? APP_MODE_LOCAL : APP_MODE_NONE;
}

const char* mode_id_from_app_mode(AppMode mode)
{
    if (mode == APP_MODE_NONE) return "";
    return MODE_ID_LOCAL;
}

void Hal::init()
{
    setenv("TZ", "UTC0", 1);
    tzset();

    esp_reset_reason_t reason = esp_reset_reason();
    if (reason != ESP_RST_POWERON) {
        // Non-normal power-on, perform I2C bus recovery
        i2cBusRecovery();
    }
    auto cfg          = M5.config();
    cfg.clear_display = false;
    M5.begin(cfg);
    M5.Display.setEpdMode(epd_mode_t::epd_quality);
    M5.Display.setRotation(3);
    Canvas = new M5Canvas(&M5.Display);
    Canvas->createSprite(M5.Display.width(), M5.Display.height());
    M5.Speaker.begin();
    s_spi_bus_inited = true;
    pm1.begin(&M5.In_I2C, M5PM1_DEFAULT_ADDR, M5PM1_I2C_FREQ_100K);
    pm1.setI2cConfig(0);
    pm1.pinMode(SD_DET_EN, OUTPUT);
    pm1.digitalWrite(SD_DET_EN, HIGH);
    pm1.pinMode(SD_DEC, INPUT_PULLUP);
    pm1.pinMode(EPD_EN, OUTPUT);
    pm1.digitalWrite(EPD_EN, HIGH);
    pm1.setChargeEnable(true);
    pm1.setBoostEnable(true);

    battery_ok  = pm1.readVbat(&battery_mv) == M5PM1_OK;
    vin_ok      = pm1.readVin(&vin_mv) == M5PM1_OK;
    sd_inserted = pm1.digitalRead(SD_DEC) == LOW;
    ESP_LOGI(TAG, "power battery_ok=%d battery_mv=%u vin_ok=%d vin_mv=%u", battery_ok, battery_mv, vin_ok, vin_mv);
    if (papercolor::should_shutdown_for_low_battery(battery_ok, battery_mv, vin_ok, vin_mv)) {
        ESP_LOGW(TAG, "shutting down because battery is low and USB power is absent");
        pm1.shutdown();
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    _led_status_indicate_event_group = xEventGroupCreate();
    xTaskCreate(LedStatusIndicateTask, "LedStatusIndicateTask", 4096, this, 5, &_led_status_indicate_task_handle);
    _settings_mutex    = xSemaphoreCreateMutex();
    _sht40_cache_mutex = xSemaphoreCreateMutex();
    sht40Read(&temperature, &humidity);
}

bool Hal::lowPowerModeEnabled() const
{
    return settings.low_power_mode;
}

bool Hal::isRtcWakeBoot() const
{
    return _is_rtc_wake_boot;
}

void Hal::detectWakeSource()
{
    _is_rtc_wake_boot = false;

    uint8_t wake_source = 0;
    if (pm1.getWakeSource(&wake_source, M5PM1_CLEAN_NONE) != M5PM1_OK) {
        return;
    }

    _is_rtc_wake_boot = (wake_source & M5PM1_WAKE_SRC_EXT_WAKE) != 0;
}

bool Hal::configureRtcWakePin()
{
    if (pm1.gpioSetFunc(RTC_WAKE_GPIO, M5PM1_GPIO_FUNC_WAKE) != M5PM1_OK) return false;
    if (pm1.gpioSetPull(RTC_WAKE_GPIO, M5PM1_GPIO_PULL_UP) != M5PM1_OK) return false;
    if (pm1.gpioSetWakeEdge(RTC_WAKE_GPIO, M5PM1_GPIO_WAKE_FALLING) != M5PM1_OK) return false;
    if (pm1.gpioSetWakeEnable(RTC_WAKE_GPIO, true) != M5PM1_OK) return false;
    return true;
}

bool Hal::scheduleNextWakeMinutes(int interval_minutes)
{
    if (interval_minutes <= 0) return false;

    m5::rtc_date_t date;
    m5::rtc_time_t time;
    if (!M5.Rtc.getDateTime(&date, &time)) return false;

    struct tm tm_now = {};
    tm_now.tm_year   = date.year - 1900;
    tm_now.tm_mon    = date.month - 1;
    tm_now.tm_mday   = date.date;
    tm_now.tm_hour   = time.hours;
    tm_now.tm_min    = time.minutes + interval_minutes;
    tm_now.tm_sec    = time.seconds;
    if (mktime(&tm_now) < 0) return false;

    if (M5.Rtc.setAlarmIRQ(&tm_now) <= 0) return false;
    return true;
}

int64_t Hal::rtcEpochUtc()
{
    m5::rtc_date_t date;
    m5::rtc_time_t time;
    if (!M5.Rtc.getDateTime(&date, &time)) return -1;

    struct tm value = {};
    value.tm_year   = date.year - 1900;
    value.tm_mon    = date.month - 1;
    value.tm_mday   = date.date;
    value.tm_hour   = time.hours;
    value.tm_min    = time.minutes;
    value.tm_sec    = time.seconds;
    value.tm_isdst  = 0;
    time_t epoch    = mktime(&value);
    return epoch < 0 ? -1 : static_cast<int64_t>(epoch);
}

bool Hal::setRtcEpochUtc(int64_t epoch_seconds)
{
    if (epoch_seconds <= 0) return false;
    time_t epoch = static_cast<time_t>(epoch_seconds);
    struct tm value;
    if (gmtime_r(&epoch, &value) == nullptr) return false;
    M5.Rtc.setDateTime(&value);
    return true;
}

bool Hal::scheduleWakeEpochUtc(int64_t epoch_seconds)
{
    int64_t now = rtcEpochUtc();

    // RX8130 countdown IRQs can fail to re-arm after an IRQ-driven PMIC wake.
    // Its calendar alarm is the proven repeated-wake path on PaperColor. Wake
    // at the start of the requested minute and wait for the remaining seconds.
    // If the minute boundary is too close, keep the board on instead of risking
    // that the IRQ fires before the PMIC has completed shutdown.
    time_t alarm_epoch = static_cast<time_t>(papercolor::select_calendar_alarm_epoch(now, epoch_seconds));
    if (alarm_epoch <= 0) return false;
    struct tm alarm;
    if (gmtime_r(&alarm_epoch, &alarm) == nullptr) return false;
    return M5.Rtc.setAlarmIRQ(&alarm) > 0;
}

bool Hal::externalPowerPresent()
{
    uint16_t vin_mv = 0;
    if (pm1.readVin(&vin_mv) != M5PM1_OK) {
        ESP_LOGW(TAG, "VIN read failed; keeping the device powered on");
        return true;
    }
    return vin_mv >= 4000;
}

void Hal::clearWakeFlags()
{
    uint8_t src = 0;
    pm1.getWakeSource(&src, M5PM1_CLEAN_ALL);
    M5.Rtc.clearIRQ();
    pm1.timerClear();
}

bool Hal::powerOff()
{
    return pm1.sysCmd(M5PM1_SYS_CMD_OFF) == M5PM1_OK;
}

void Hal::settingsInit()
{
    ESP_LOGI(TAG, "settingsInit start");
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    // Defaults (first boot / garbage NVS)
    memset(&settings, 0, sizeof(settings));
    settings.rotation             = 1;
    settings.auto_slideshow       = false;
    settings.interval_minutes     = 60;
    settings.boot_sound           = true;
    settings.low_power_mode       = false;
    settings.gateway_enabled      = false;
    settings.gateway_poll_minutes = 15;
    cstring_copy(settings.current_mode, "", sizeof(settings.current_mode));
    cstring_copy(settings.device_name, "papercolor", sizeof(settings.device_name));

    bool mode_changed        = false;
    bool device_name_changed = false;
    bool has_nvs             = false;
    nvs_handle_t nvs_handle;
    if (nvs_open("papercolor", NVS_READONLY, &nvs_handle) == ESP_OK) {
        has_nvs = true;
        size_t value_size;

        value_size = sizeof(settings.wifi_ssid);
        nvs_get_str(nvs_handle, "wifi_ssid", settings.wifi_ssid, &value_size);

        value_size = sizeof(settings.wifi_password);
        nvs_get_str(nvs_handle, "wifi_pass", settings.wifi_password, &value_size);

        uint8_t value_u8;
        if (nvs_get_u8(nvs_handle, "rotation", &value_u8) == ESP_OK && value_u8 <= 1) settings.rotation = value_u8;

        if (nvs_get_u8(nvs_handle, "auto_slide", &value_u8) == ESP_OK) settings.auto_slideshow = (value_u8 != 0);

        if (nvs_get_u8(nvs_handle, "boot_sound", &value_u8) == ESP_OK) settings.boot_sound = (value_u8 != 0);
        if (nvs_get_u8(nvs_handle, "low_power", &value_u8) == ESP_OK) settings.low_power_mode = (value_u8 != 0);
        if (nvs_get_u8(nvs_handle, "gw_enabled", &value_u8) == ESP_OK) settings.gateway_enabled = (value_u8 != 0);

        uint16_t value_u16;
        if (nvs_get_u16(nvs_handle, "interval", &value_u16) == ESP_OK && value_u16 >= 1 && value_u16 <= 255)
            settings.interval_minutes = (int)value_u16;
        if (nvs_get_u16(nvs_handle, "gw_poll", &value_u16) == ESP_OK && value_u16 >= 1 && value_u16 <= 1440)
            settings.gateway_poll_minutes = value_u16;

        value_size = sizeof(settings.gateway_url);
        nvs_get_str(nvs_handle, "gw_url", settings.gateway_url, &value_size);

        value_size = sizeof(settings.gateway_token);
        nvs_get_str(nvs_handle, "gw_token", settings.gateway_token, &value_size);

        value_size         = sizeof(settings.current_mode);
        esp_err_t mode_err = nvs_get_str(nvs_handle, "cur_mode", settings.current_mode, &value_size);
        if (mode_err == ESP_OK) {
            char normalized_mode[sizeof(settings.current_mode)];
            mode_changed = normalize_mode_id(settings.current_mode, normalized_mode, sizeof(normalized_mode));
            cstring_copy(settings.current_mode, normalized_mode, sizeof(settings.current_mode));
        }

        value_size = sizeof(settings.device_name);
        if (nvs_get_str(nvs_handle, "device_name", settings.device_name, &value_size) == ESP_OK) {
            char normalized_device_name[sizeof(settings.device_name)];
            device_name_changed =
                normalize_device_name(settings.device_name, normalized_device_name, sizeof(normalized_device_name));
            cstring_copy(settings.device_name, normalized_device_name, sizeof(settings.device_name));
        }

        nvs_close(nvs_handle);
    }

    if (!has_nvs) {
        char normalized_mode[sizeof(settings.current_mode)];
        mode_changed = normalize_mode_id(settings.current_mode, normalized_mode, sizeof(normalized_mode));
        cstring_copy(settings.current_mode, normalized_mode, sizeof(settings.current_mode));
    }

    {
        char normalized_device_name[sizeof(settings.device_name)];
        device_name_changed =
            normalize_device_name(settings.device_name, normalized_device_name, sizeof(normalized_device_name));
        cstring_copy(settings.device_name, normalized_device_name, sizeof(settings.device_name));
    }

    if (mode_changed) {
        settingsSave(SETTING_CURRENT_MODE);
    }

    if (device_name_changed) {
        settingsSave(SETTING_DEVICE_NAME);
    }

    Canvas->setRotation(settings.rotation);

    ESP_LOGI(TAG, "Settings loaded: rot=%d, slide=%d, interval=%d, mode=%s, boot_sound=%d, gateway=%d, gw_poll=%u",
             settings.rotation, settings.auto_slideshow, settings.interval_minutes, settings.current_mode,
             settings.boot_sound ? 1 : 0, settings.gateway_enabled ? 1 : 0, settings.gateway_poll_minutes);
}

void Hal::settingsSave(SettingKey key)
{
    nvs_handle_t h;
    if (nvs_open("papercolor", NVS_READWRITE, &h) != ESP_OK) return;

    bool changed = false;

    switch (key) {
        case SETTING_WIFI_SSID: {
            char prev[64] = {};
            size_t sz     = sizeof(prev);
            if (nvs_get_str(h, "wifi_ssid", prev, &sz) != ESP_OK || cstring_compare(prev, settings.wifi_ssid) != 0) {
                nvs_set_str(h, "wifi_ssid", settings.wifi_ssid);
                changed = true;
            }
            break;
        }
        case SETTING_WIFI_PASSWORD: {
            char prev[64] = {};
            size_t sz     = sizeof(prev);
            if (nvs_get_str(h, "wifi_pass", prev, &sz) != ESP_OK ||
                cstring_compare(prev, settings.wifi_password) != 0) {
                nvs_set_str(h, "wifi_pass", settings.wifi_password);
                changed = true;
            }
            break;
        }
        case SETTING_ROTATION: {
            uint8_t v;
            if (nvs_get_u8(h, "rotation", &v) != ESP_OK || v != settings.rotation) {
                nvs_set_u8(h, "rotation", settings.rotation);
                changed = true;
            }
            break;
        }
        case SETTING_AUTO_SLIDESHOW: {
            uint8_t v;
            if (nvs_get_u8(h, "auto_slide", &v) != ESP_OK || (v != 0) != settings.auto_slideshow) {
                nvs_set_u8(h, "auto_slide", settings.auto_slideshow ? 1 : 0);
                changed = true;
            }
            break;
        }
        case SETTING_INTERVAL: {
            uint16_t v;
            if (nvs_get_u16(h, "interval", &v) != ESP_OK || v != (uint16_t)settings.interval_minutes) {
                nvs_set_u16(h, "interval", (uint16_t)settings.interval_minutes);
                changed = true;
            }
            break;
        }
        case SETTING_CURRENT_MODE: {
            char normalized_mode[sizeof(settings.current_mode)];
            normalize_mode_id(settings.current_mode, normalized_mode, sizeof(normalized_mode));
            cstring_copy(settings.current_mode, normalized_mode, sizeof(settings.current_mode));

            char prev[16] = {};
            size_t sz     = sizeof(prev);
            if (nvs_get_str(h, "cur_mode", prev, &sz) != ESP_OK || cstring_compare(prev, settings.current_mode) != 0) {
                nvs_set_str(h, "cur_mode", settings.current_mode);
                changed = true;
            }
            break;
        }
        case SETTING_BOOT_SOUND: {
            uint8_t v;
            if (nvs_get_u8(h, "boot_sound", &v) != ESP_OK || (v != 0) != settings.boot_sound) {
                nvs_set_u8(h, "boot_sound", settings.boot_sound ? 1 : 0);
                changed = true;
            }
            break;
        }
        case SETTING_LOW_POWER_MODE: {
            uint8_t v;
            if (nvs_get_u8(h, "low_power", &v) != ESP_OK || (v != 0) != settings.low_power_mode) {
                nvs_set_u8(h, "low_power", settings.low_power_mode ? 1 : 0);
                changed = true;
            }
            break;
        }
        case SETTING_DEVICE_NAME: {
            char normalized_device_name[sizeof(settings.device_name)];
            normalize_device_name(settings.device_name, normalized_device_name, sizeof(normalized_device_name));
            cstring_copy(settings.device_name, normalized_device_name, sizeof(settings.device_name));

            char prev[64] = {};
            size_t sz     = sizeof(prev);
            if (nvs_get_str(h, "device_name", prev, &sz) != ESP_OK ||
                cstring_compare(prev, settings.device_name) != 0) {
                nvs_set_str(h, "device_name", settings.device_name);
                changed = true;
            }
            break;
        }
        case SETTING_GATEWAY_URL: {
            char prev[sizeof(settings.gateway_url)] = {};
            size_t sz                               = sizeof(prev);
            if (nvs_get_str(h, "gw_url", prev, &sz) != ESP_OK || cstring_compare(prev, settings.gateway_url) != 0) {
                nvs_set_str(h, "gw_url", settings.gateway_url);
                changed = true;
            }
            break;
        }
        case SETTING_GATEWAY_TOKEN: {
            char prev[sizeof(settings.gateway_token)] = {};
            size_t sz                                 = sizeof(prev);
            if (nvs_get_str(h, "gw_token", prev, &sz) != ESP_OK || cstring_compare(prev, settings.gateway_token) != 0) {
                nvs_set_str(h, "gw_token", settings.gateway_token);
                changed = true;
            }
            break;
        }
        case SETTING_GATEWAY_ENABLED: {
            uint8_t value;
            if (nvs_get_u8(h, "gw_enabled", &value) != ESP_OK || (value != 0) != settings.gateway_enabled) {
                nvs_set_u8(h, "gw_enabled", settings.gateway_enabled ? 1 : 0);
                changed = true;
            }
            break;
        }
        case SETTING_GATEWAY_POLL_MINUTES: {
            uint16_t value;
            if (nvs_get_u16(h, "gw_poll", &value) != ESP_OK || value != settings.gateway_poll_minutes) {
                nvs_set_u16(h, "gw_poll", settings.gateway_poll_minutes);
                changed = true;
            }
            break;
        }
    }

    if (changed) {
        nvs_commit(h);
        ESP_LOGI(TAG, "Settings: key %d saved", (int)key);
    }
    nvs_close(h);
}

void Hal::settingsLock()
{
    if (_settings_mutex) xSemaphoreTake(_settings_mutex, portMAX_DELAY);
}

void Hal::settingsUnlock()
{
    if (_settings_mutex) xSemaphoreGive(_settings_mutex);
}

void Hal::update()
{
    M5.update();

    uint32_t now = millis_();
    if ((_sht40_last_attempt_ms == 0 && now >= 2000) ||
        (_sht40_last_attempt_ms != 0 && now - _sht40_last_attempt_ms >= 30000)) {
        _sht40_last_attempt_ms = now;
        float sample_temp      = 0.0f;
        float sample_humi      = 0.0f;
        sht40Read(&sample_temp, &sample_humi);
    }
}

void Hal::getDeviceMac(uint8_t* mac)
{
    esp_read_mac(mac, ESP_MAC_EFUSE_FACTORY);
}

bool Hal::isSDCardInserted()
{
    return pm1.digitalRead(SD_DEC) == LOW ? true : false;
}

bool Hal::sht40Read(float* temp, float* humi)
{
    if (temp == nullptr || humi == nullptr) return false;

    uint8_t cmd    = SHT4X_CMD_HI_PRE;
    uint8_t buf[6] = {0};

    if (!M5.In_I2C.start(SHT4X_ADDR, false, 400000)) {
        M5.In_I2C.stop();
        return false;
    }
    bool write_ok = M5.In_I2C.write(&cmd, 1);
    M5.In_I2C.stop();
    if (!write_ok) return false;

    vTaskDelay(pdMS_TO_TICKS(10));  // High precision mode requires ~8.3ms

    if (!M5.In_I2C.start(SHT4X_ADDR, true, 400000)) {
        M5.In_I2C.stop();
        return false;
    }
    bool read_ok = M5.In_I2C.read(buf, 6, true);
    M5.In_I2C.stop();
    if (!read_ok || sht4x_crc8(buf, 2) != buf[2] || sht4x_crc8(buf + 3, 2) != buf[5]) return false;

    uint16_t raw_t = (buf[0] << 8) | buf[1];
    uint16_t raw_h = (buf[3] << 8) | buf[4];

    float sample_temp = -45.0f + 175.0f * raw_t / 65535.0f;
    float sample_humi = -6.0f + 125.0f * raw_h / 65535.0f;
    if (sample_humi > 100.0f) sample_humi = 100.0f;
    if (sample_humi < 0.0f) sample_humi = 0.0f;

    *temp = sample_temp;
    *humi = sample_humi;
    if (_sht40_cache_mutex && xSemaphoreTake(_sht40_cache_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        temperature        = sample_temp;
        humidity           = sample_humi;
        _sht40_cache_valid = true;
        xSemaphoreGive(_sht40_cache_mutex);
    }

    return true;
}

bool Hal::sht40CachedRead(float* temp, float* humi)
{
    if (temp == nullptr || humi == nullptr || _sht40_cache_mutex == nullptr) return false;
    if (xSemaphoreTake(_sht40_cache_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return false;
    bool valid = _sht40_cache_valid;
    if (valid) {
        *temp = temperature;
        *humi = humidity;
    }
    xSemaphoreGive(_sht40_cache_mutex);
    return valid;
}

bool Hal::microphoneDiagnostics(MicrophoneDiagnostics* result)
{
    if (result == nullptr || !M5.Mic.isEnabled()) return false;

    static constexpr size_t k_sample_count  = 1024;
    static constexpr uint32_t k_sample_rate = 16000;
    int16_t warmup[k_sample_count]          = {0};
    int16_t samples[k_sample_count]         = {0};

    M5.Speaker.end();
    if (!M5.Mic.begin()) {
        M5.Speaker.begin();
        return false;
    }

    auto capture = [&](int16_t* buffer) {
        bool queued         = M5.Mic.record(buffer, k_sample_count, k_sample_rate, false);
        uint32_t started_ms = millis_();
        while (queued && M5.Mic.isRecording() && millis_() - started_ms < 500) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        return queued && !M5.Mic.isRecording();
    };

    // The codec and I2S clocks need one block to settle after power-up.
    bool completed = capture(warmup) && capture(samples);
    M5.Mic.end();
    M5.Speaker.begin();
    if (!completed) return false;

    int64_t sum = 0;
    for (size_t i = 0; i < k_sample_count; ++i) {
        sum += samples[i];
    }
    int32_t mean = static_cast<int32_t>(sum / static_cast<int64_t>(k_sample_count));

    uint32_t peak       = 0;
    uint64_t square_sum = 0;
    for (size_t i = 0; i < k_sample_count; ++i) {
        int32_t centered  = static_cast<int32_t>(samples[i]) - mean;
        uint32_t absolute = static_cast<uint32_t>(centered < 0 ? -centered : centered);
        if (absolute > peak) peak = absolute;
        square_sum += static_cast<int64_t>(centered) * centered;
    }

    result->sample_count = k_sample_count;
    result->dc_offset    = mean;
    result->peak         = peak;
    result->rms          = static_cast<uint32_t>(std::sqrt(static_cast<double>(square_sum) / k_sample_count));
    return true;
}

size_t Hal::rgbLedSelfTest()
{
    size_t count = M5.Led.getCount();
    if (count == 0) return 0;

    if (_led_status_indicate_task_handle != nullptr) {
        vTaskSuspend(_led_status_indicate_task_handle);
    }

    M5.Led.setBrightness(80);
    M5.Led.setAllColor(0, 0, 0);
    for (size_t i = 0; i < count; ++i) {
        M5.Led.setAllColor(0, 0, 0);
        M5.Led.setColor(i, 255, 255, 255);
        M5.Led.display();
        vTaskDelay(pdMS_TO_TICKS(300));
    }

    static constexpr uint8_t colors[][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}};
    for (const auto& color : colors) {
        M5.Led.setAllColor(color[0], color[1], color[2]);
        M5.Led.display();
        vTaskDelay(pdMS_TO_TICKS(250));
    }

    M5.Led.setAllColor(0, 0, 0);
    M5.Led.display();
    if (_led_status_indicate_task_handle != nullptr) {
        vTaskResume(_led_status_indicate_task_handle);
    }
    return count;
}

bool Hal::speakerSelfTest()
{
    if (!M5.Speaker.isEnabled() && !M5.Speaker.begin()) return false;

    uint8_t previous_volume = M5.Speaker.getVolume();
    M5.Speaker.setVolume(96);
    bool first = M5.Speaker.tone(880, 160);
    vTaskDelay(pdMS_TO_TICKS(190));
    bool second = M5.Speaker.tone(1320, 180);
    vTaskDelay(pdMS_TO_TICKS(210));
    M5.Speaker.setVolume(previous_volume);
    return first && second;
}

/**
 * @brief  Write one byte to RX8130CE RAM
 * @param  index  Address index 0~3 (corresponding to registers 0x20~0x23)
 * @param  value  Value to write, 0~255
 * @return true on success
 */
bool Hal::rx8130RamWrite(uint8_t index, uint8_t value)
{
    if (index >= RX8130_RAM_SIZE) return false;

    uint8_t buf[2];
    buf[0] = RX8130_RAM_BASE + index;  // Register address
    buf[1] = value;                    // Data

    if (!M5.In_I2C.start(RX8130_ADDR, false, 400000)) {
        M5.In_I2C.stop();
        return false;
    }
    M5.In_I2C.write(buf, 2);
    M5.In_I2C.stop();
    return true;
}

/**
 * @brief  Read one byte from RX8130CE RAM
 * @param  index  Address index 0~3 (corresponding to registers 0x20~0x23)
 * @param  value  Pointer to store the read value
 * @return true on success
 */
bool Hal::rx8130RamRead(uint8_t index, uint8_t* value)
{
    if (index >= RX8130_RAM_SIZE || value == nullptr) return false;

    uint8_t reg = RX8130_RAM_BASE + index;

    // Write the register address first
    if (!M5.In_I2C.start(RX8130_ADDR, false, 400000)) {
        M5.In_I2C.stop();
        return false;
    }
    M5.In_I2C.write(&reg, 1);
    M5.In_I2C.stop();

    // Then read the data
    if (!M5.In_I2C.start(RX8130_ADDR, true, 400000)) {
        M5.In_I2C.stop();
        return false;
    }
    M5.In_I2C.read(value, 1);
    M5.In_I2C.stop();
    return true;
}

void Hal::i2cBusRecovery()
{
    ESP_LOGW(TAG, "Performing I2C bus recovery");

    gpio_config_t cfg;
    cfg.pin_bit_mask = (1ULL << SYS_SCL_PIN) | (1ULL << SYS_SDA_PIN);
    cfg.mode         = GPIO_MODE_INPUT_OUTPUT_OD;
    cfg.pull_up_en   = GPIO_PULLUP_ENABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type    = GPIO_INTR_DISABLE;
    gpio_config(&cfg);

    // Release both lines
    gpio_set_level(SYS_SDA_PIN, 1);
    gpio_set_level(SYS_SCL_PIN, 1);
    esp_rom_delay_us(10);

    // Send 9 clock pulses
    for (int i = 0; i < 9; i++) {
        gpio_set_level(SYS_SCL_PIN, 0);
        esp_rom_delay_us(5);
        gpio_set_level(SYS_SCL_PIN, 1);
        esp_rom_delay_us(5);

        // Exit early if SDA has already been released
        if (gpio_get_level(SYS_SDA_PIN) == 1) {
            break;
        }
    }

    gpio_set_level(SYS_SDA_PIN, 0);
    esp_rom_delay_us(5);
    gpio_set_level(SYS_SCL_PIN, 1);
    esp_rom_delay_us(5);
    gpio_set_level(SYS_SDA_PIN, 1);
    esp_rom_delay_us(5);

    gpio_reset_pin(SYS_SCL_PIN);
    gpio_reset_pin(SYS_SDA_PIN);
}

bool Hal::statusEventSend(EventBits_t event_bits)
{
    if (_led_status_indicate_event_group == nullptr) return false;
    xEventGroupSetBits(_led_status_indicate_event_group, event_bits);
    return true;
}

void Hal::LedStatusIndicateTask(void* task_parameters)
{
    Hal* instance = static_cast<Hal*>(task_parameters);
    EventBits_t event_bits;
    const EventBits_t all_event_bits = OPERATION_EVENT_FAILED | OPERATION_EVENT_ERROR_IMAGE_READ |
                                       OPERATION_EVENT_REFRESH_START | OPERATION_EVENT_REFRESH_COMPLETE |
                                       OPERATION_EVENT_SUCCESS | OPERATION_EVENT_WAITING_WIFI |
                                       OPERATION_EVENT_STARTUP_SUCCESS;

    LedState current_state    = LED_STATE_IDLE;
    LedState previous_state   = LED_STATE_IDLE;
    uint32_t state_start_time = 0;
    uint32_t blink_phase      = 0;

    while (1) {
        static bool startup_animation_played = false;
        if (!startup_animation_played) {
            const uint32_t rainbow_colors[] = {0xFF0066, 0xFFCC00, 0x006633, 0x336699, 0x990099};
            const int color_count           = sizeof(rainbow_colors) / sizeof(rainbow_colors[0]);

            for (int color_index = 0; color_index < color_count; color_index++) {
                uint8_t red   = (rainbow_colors[color_index] >> 16) & 0xFF;
                uint8_t green = (rainbow_colors[color_index] >> 8) & 0xFF;
                uint8_t blue  = rainbow_colors[color_index] & 0xFF;

                for (int brightness = 0; brightness <= 95; brightness += 3) {
                    M5.Led.setBrightness(brightness);
                    M5.Led.setAllColor(red, green, blue);
                    M5.Led.display();
                    vTaskDelay(pdMS_TO_TICKS(12));
                }
                vTaskDelay(pdMS_TO_TICKS(30));
                for (int brightness = 95; brightness >= 0; brightness -= 3) {
                    M5.Led.setBrightness(brightness);
                    M5.Led.setAllColor(red, green, blue);
                    M5.Led.display();
                    vTaskDelay(pdMS_TO_TICKS(12));
                }
                vTaskDelay(pdMS_TO_TICKS(30));
            }

            for (int flash_index = 0; flash_index < 2; flash_index++) {
                M5.Led.setBrightness(100);
                M5.Led.setAllColor(0, 255, 0);
                M5.Led.display();
                vTaskDelay(pdMS_TO_TICKS(200));
                M5.Led.setBrightness(0);
                M5.Led.display();
                vTaskDelay(pdMS_TO_TICKS(200));
            }

            startup_animation_played = true;
        }

        event_bits = xEventGroupWaitBits(instance->_led_status_indicate_event_group, all_event_bits, pdTRUE, pdFALSE,
                                         pdMS_TO_TICKS(10));

        uint32_t now = millis_();

        if (event_bits & OPERATION_EVENT_FAILED) {
            ESP_LOGW(TAG, "Event: Directory open error");
            if (current_state != LED_STATE_ERROR_FADE) {
                previous_state = current_state;
            }
            current_state    = LED_STATE_ERROR_FADE;
            state_start_time = now;
        }
        if (event_bits & OPERATION_EVENT_ERROR_IMAGE_READ) {
            ESP_LOGE(TAG, "Event: Image read error");
            if (current_state != LED_STATE_ERROR_FADE) {
                previous_state = current_state;
            }
            current_state    = LED_STATE_ERROR_FADE;
            state_start_time = now;
        }
        if (event_bits & OPERATION_EVENT_REFRESH_START) {
            ESP_LOGI(TAG, "Event: Refresh start");
            if (current_state != LED_STATE_ERROR_FADE) {
                previous_state = current_state;
            }
            current_state    = LED_STATE_REFRESH_START_BLINK;
            state_start_time = now;
            blink_phase      = 0;
        }
        if (event_bits & OPERATION_EVENT_REFRESH_COMPLETE) {
            ESP_LOGI(TAG, "Event: Refresh complete");
            if (current_state != LED_STATE_ERROR_FADE) {
                previous_state = current_state;
            }
            current_state    = LED_STATE_REFRESH_COMPLETE_BLINK;
            state_start_time = now;
            blink_phase      = 0;
        }
        if (event_bits & OPERATION_EVENT_SUCCESS) {
            ESP_LOGI(TAG, "Event: Operation success");
            if (current_state != LED_STATE_ERROR_FADE) {
                previous_state = current_state;
            }
            current_state    = LED_STATE_SUCCESS_FADE;
            state_start_time = now;
        }
        if (event_bits & OPERATION_EVENT_WAITING_WIFI) {
            ESP_LOGI(TAG, "Event: Waiting for WiFi connection");
            if (current_state != LED_STATE_ERROR_FADE) {
                previous_state = current_state;
            }
            current_state    = LED_STATE_WAITING_WIFI_BLINK;
            state_start_time = now;
            blink_phase      = 0;
        }
        if (event_bits & OPERATION_EVENT_STARTUP_SUCCESS) {
            ESP_LOGI(TAG, "Event: Startup success");
            if (current_state != LED_STATE_ERROR_FADE) {
                previous_state = current_state;
            }
            current_state    = LED_STATE_SUCCESS_STEADY_BLINK;
            state_start_time = now;
        }

        uint32_t elapsed = now - state_start_time;
        int brightness   = 0;

        switch (current_state) {
            case LED_STATE_SUCCESS_FADE:
                if (elapsed < 1050) {
                    brightness = 100 - (elapsed / 50) * 5;
                    brightness = (brightness < 0) ? 0 : brightness;
                    M5.Led.setBrightness(brightness);
                    M5.Led.setAllColor(0, 255, 0);
                    M5.Led.display();
                } else {
                    current_state    = LED_STATE_SUCCESS_STEADY_BLINK;
                    state_start_time = now;
                }
                break;

            case LED_STATE_ERROR_FADE:
                if (elapsed < 1050) {
                    brightness = 100 - (elapsed / 50) * 5;
                    brightness = (brightness < 0) ? 0 : brightness;
                    M5.Led.setBrightness(brightness);
                    M5.Led.setAllColor(255, 0, 0);
                    M5.Led.display();
                } else {
                    if (previous_state != LED_STATE_IDLE) {
                        current_state = previous_state;
                    } else {
                        current_state = LED_STATE_IDLE;
                    }
                    state_start_time = now;
                }
                break;

            case LED_STATE_REFRESH_START_BLINK:
                blink_phase = elapsed / 100;
                if (blink_phase < 4) {
                    brightness = (blink_phase % 2 == 0) ? 100 : 0;
                    M5.Led.setBrightness(brightness);
                    M5.Led.setAllColor(0, 255, 255);
                    M5.Led.display();
                } else if (elapsed < 2400) {
                    M5.Led.setBrightness(0);
                    M5.Led.display();
                } else {
                    current_state    = LED_STATE_REFRESH_START_STEADY_BLINK;
                    state_start_time = now;
                }
                break;

            case LED_STATE_REFRESH_COMPLETE_BLINK:
                blink_phase = elapsed / 100;
                if (blink_phase < 4) {
                    brightness = (blink_phase % 2 == 0) ? 100 : 0;
                    M5.Led.setBrightness(brightness);
                    M5.Led.setAllColor(0, 255, 0);
                    M5.Led.display();
                } else {
                    current_state    = LED_STATE_SUCCESS_STEADY_BLINK;
                    state_start_time = now;
                }
                break;

            case LED_STATE_WAITING_WIFI_BLINK:
                if (elapsed % 1000 < 100) {
                    M5.Led.setBrightness(100);
                    M5.Led.setAllColor(255, 0, 0);
                } else {
                    M5.Led.setBrightness(0);
                }
                M5.Led.display();
                break;

            case LED_STATE_SUCCESS_STEADY_BLINK:
                if (elapsed >= 2000 && (elapsed - 2000) % 2000 < 50) {
                    M5.Led.setBrightness(50);
                    M5.Led.setAllColor(0, 255, 0);
                } else {
                    M5.Led.setBrightness(0);
                }
                M5.Led.display();
                break;

            case LED_STATE_REFRESH_START_STEADY_BLINK:
                if (elapsed >= 2000 && (elapsed - 2000) % 2000 < 50) {
                    M5.Led.setBrightness(50);
                    M5.Led.setAllColor(0, 255, 255);
                } else {
                    M5.Led.setBrightness(0);
                }
                M5.Led.display();
                break;

            case LED_STATE_REFRESH_COMPLETE_STEADY_BLINK:
                if (elapsed % 2000 < 100) {
                    M5.Led.setBrightness(100);
                    M5.Led.setAllColor(0, 255, 0);
                } else {
                    M5.Led.setBrightness(0);
                }
                M5.Led.display();
                break;

            case LED_STATE_IDLE:
            default:
                break;
        }
    }

    M5.Led.setBrightness(0);
    M5.Led.display();
    instance->_led_status_indicate_task_handle = nullptr;
    vTaskDelete(nullptr);
}
