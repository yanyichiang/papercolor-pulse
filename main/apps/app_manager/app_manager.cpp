/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_manager.h"
#include "button_mapping.h"
#include "hal/hal.h"
#include "hal/wifi/hal_wifi.h"
#include "hal/utils/audio/audio.h"
#include "apps/local_photo_slideshow/local_photo_slideshow.h"
#include "apps/app_server/app_server.h"
#include "apps/scheduled_push/scheduled_push.h"
#include "apps/voice_capture/voice_capture.h"
#include "esp_log.h"
#include <esp_timer.h>
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <M5Unified.h>
#include "qrcode.h"
#include "hal/utils/image/image_utils.h"
#include "hal/storage/hal_storage.h"

#ifndef APP_ASSETS_USE_EMBEDDED
#define APP_ASSETS_USE_EMBEDDED 0
#endif

#if APP_ASSETS_USE_EMBEDDED
extern const uint8_t _binary_PaperColor_png_start[] asm("_binary_PaperColor_png_start");
extern const uint8_t _binary_PaperColor_png_end[] asm("_binary_PaperColor_png_end");
extern const uint8_t _binary_arrowClickToPowerOn_png_start[] asm("_binary_arrowClickToPowerOn_png_start");
extern const uint8_t _binary_arrowClickToPowerOn_png_end[] asm("_binary_arrowClickToPowerOn_png_end");
#endif

using namespace hal_wifi;

static const char* g_tag = "AppMgr";

// ---- Global objects ----
PhotoSlideshow photo_slideshow;

// ---- Mode state ----
static AppMode g_current_mode     = APP_MODE_LOCAL;
static uint32_t g_btn_press_start = 0;
static bool g_btn_long_pressed    = false;

// ---- WiFi AP auto-off timer ----
static uint32_t g_ap_auto_off_timer     = 0;
static bool g_ap_auto_off_timer_running = false;

// ---- WiFi auto-reconnect ----
static uint32_t g_wifi_reconnect_last_try_ms         = 0;
static constexpr uint32_t WIFI_RECONNECT_INTERVAL_MS = 10000;
static bool g_wifi_reconnect_paused                  = false;
static uint32_t g_low_power_last_activity_ms         = 0;
static bool g_refresh_in_progress                    = false;
static std::atomic<uint32_t> g_active_operations{0};
static constexpr uint32_t LOW_POWER_IDLE_SHUTDOWN_MS = 60000;

// ---- millis() ----
static inline uint32_t millis_()
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static bool mode_requires_sta(AppMode mode)
{
    // STA is now brought up for any active mode so the LAN-served web portal
    // stays reachable. Previously this was true only for the (now-removed)
    // cloud mode and false for LOCAL, which silently prevented LOCAL boots
    // from connecting STA at all even when saved credentials existed.
    return mode != APP_MODE_NONE;
}

static void get_ap_name(char* ap_name, size_t ap_name_size)
{
    uint8_t mac[6];
    hal.getDeviceMac(mac);
    snprintf(ap_name, ap_name_size, "PaperColor-%02X%02X%02X", mac[3], mac[4], mac[5]);
}

static esp_err_t ensure_apsta_started()
{
    if (WiFi.getMode() == WiFiMode::APSTA) {
        return ESP_OK;
    }

    char ap_name[32];
    get_ap_name(ap_name, sizeof(ap_name));

    esp_err_t err = WiFi.setMode(WiFiMode::APSTA);
    if (err != ESP_OK) {
        return err;
    }

    err = WiFi.softAP(ap_name, "", 6);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(g_tag, "AP SSID: %s", ap_name);
    ESP_LOGI(g_tag, "AP IP : %s", WiFi.softAPIP().c_str());
    ESP_LOGI(g_tag, "AP MAC : %s", WiFi.macAddressAP().c_str());
    return ESP_OK;
}

static esp_err_t disconnect_sta_keep_ap_internal()
{
    g_wifi_reconnect_last_try_ms = 0;
    g_ap_auto_off_timer_running  = false;

    esp_err_t err = ensure_apsta_started();
    if (err != ESP_OK) {
        return err;
    }

    err = WiFi.disconnect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_CONNECT) {
        return err;
    }

    return ESP_OK;
}

static esp_err_t ensure_sta_connected_if_needed(bool wait_for_connect)
{
    esp_err_t err = ensure_apsta_started();
    if (err != ESP_OK) {
        return err;
    }

    if (!mode_requires_sta(g_current_mode) || !hal.settings.wifi_ssid[0]) {
        return ESP_OK;
    }

    uint32_t timeout_ms = wait_for_connect ? 15000 : 0;
    err                 = WiFi.connect(hal.settings.wifi_ssid, hal.settings.wifi_password, timeout_ms);
    if (err == ESP_OK && wait_for_connect) {
        ESP_LOGI(g_tag, "STA IP : %s", WiFi.localIP().c_str());
    }
    return err;
}

// ---- Mode management ----
static void apply_current_mode_setting()
{
    cstring_copy(hal.settings.current_mode, mode_id_from_app_mode(g_current_mode), sizeof(hal.settings.current_mode));
    app_manager_set_current_mode(hal.settings.current_mode);
}

static void start_current_mode()
{
    if (g_current_mode == APP_MODE_NONE) {
        return;
    } else {
        photo_slideshow.start();
        ESP_LOGI(g_tag, "Started LOCAL mode");
    }
}

static bool prepare_next_low_power_wake()
{
    if (!hal.lowPowerModeEnabled()) {
        return false;
    }
    if (!hal.settings.auto_slideshow || hal.settings.interval_minutes <= 0) {
        return false;
    }

    if (!hal.configureRtcWakePin()) {
        ESP_LOGW(g_tag, "Failed to configure RTC wake pin");
        return false;
    }
    if (!hal.scheduleNextWakeMinutes(hal.settings.interval_minutes)) {
        ESP_LOGW(g_tag, "Failed to schedule next RTC wake");
        return false;
    }
    return true;
}

static bool has_user_activity()
{
    return M5.BtnA.wasPressed() || M5.BtnA.wasClicked() || M5.BtnA.isPressed() || M5.BtnB.wasPressed() ||
           M5.BtnB.wasClicked() || M5.BtnB.isPressed() || M5.BtnC.wasPressed() || M5.BtnC.wasClicked() ||
           M5.BtnC.isPressed();
}

void app_manager_mark_activity(void)
{
    g_low_power_last_activity_ms = millis_();
}

void app_manager_set_refresh_in_progress(bool in_progress)
{
    g_refresh_in_progress = in_progress;
}

void app_manager_operation_begin(void)
{
    g_active_operations.fetch_add(1, std::memory_order_relaxed);
    app_manager_mark_activity();
}

void app_manager_operation_end(void)
{
    uint32_t active = g_active_operations.load(std::memory_order_relaxed);
    while (active > 0 && !g_active_operations.compare_exchange_weak(active, active - 1, std::memory_order_relaxed)) {
    }
    app_manager_mark_activity();
}

static bool external_power_present_cached()
{
    static uint32_t last_check_ms = 0;
    static bool present           = true;
    uint32_t now                  = millis_();
    if (last_check_ms == 0 || now - last_check_ms >= 5000) {
        present       = hal.externalPowerPresent();
        last_check_ms = now;
    }
    return present;
}

static bool should_idle_power_off_in_low_power_mode()
{
    bool gateway_low_power = scheduled_push::enabled() && !external_power_present_cached();
    if (!hal.lowPowerModeEnabled() && !gateway_low_power) {
        g_low_power_last_activity_ms = 0;
        g_refresh_in_progress        = false;
        return false;
    }
    if (!gateway_low_power && (!hal.settings.auto_slideshow || hal.settings.interval_minutes <= 0)) {
        g_low_power_last_activity_ms = 0;
        g_refresh_in_progress        = false;
        return false;
    }

    WiFiMode current_wifi_mode = WiFi.getMode();
    bool ap_active             = (current_wifi_mode == WiFiMode::APSTA || current_wifi_mode == WiFiMode::AP);
    bool has_ap_clients        = ap_active && (WiFi.softAPgetStationNum() > 0);
    if (has_ap_clients) {
        app_manager_mark_activity();
        return false;
    }

    uint32_t now = millis_();
    if (has_user_activity()) {
        app_manager_mark_activity();
        return false;
    }

    if (g_refresh_in_progress || g_active_operations.load(std::memory_order_relaxed) > 0) {
        return false;
    }

    if (g_low_power_last_activity_ms == 0) {
        g_low_power_last_activity_ms = now;
        return false;
    }

    return now - g_low_power_last_activity_ms >= LOW_POWER_IDLE_SHUTDOWN_MS;
}

static void shutdown_after_low_power_cycle()
{
    if (scheduled_push::enabled()) {
        if (scheduled_push::schedule_next_wake_and_power_off()) return;
        ESP_LOGW(g_tag, "Scheduled gateway wake failed; keeping device powered on");
        return;
    }
    if (!prepare_next_low_power_wake()) {
        ESP_LOGW(g_tag, "Low-power shutdown skipped because wake scheduling failed");
        return;
    }

    hal.clearWakeFlags();
    vTaskDelay(pdMS_TO_TICKS(50));
    if (!hal.powerOff()) {
        ESP_LOGW(g_tag, "PMIC rejected the power-off command; keeping services available");
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGW(g_tag, "Power-off command returned without power loss; keeping services available");
}

static bool run_rtc_wake_one_shot_cycle()
{
    if (!hal.isRtcWakeBoot() || !hal.lowPowerModeEnabled() || !hal.settings.auto_slideshow ||
        hal.settings.interval_minutes <= 0) {
        return false;
    }

    ESP_LOGI(g_tag, "RTC wake low-power cycle start, mode=%s", mode_id_from_app_mode(g_current_mode));

    if (g_current_mode == APP_MODE_LOCAL) {
        photo_slideshow.init("/data", 0);
        vTaskDelay(pdMS_TO_TICKS(1000));
        bool refreshed = photo_slideshow.runOneShotRefresh();
        if (!refreshed) {
            ESP_LOGW(g_tag, "RTC one-shot local refresh did not display a photo");
        }
        shutdown_after_low_power_cycle();
    }

    shutdown_after_low_power_cycle();
    return true;
}

static void stop_current_mode()
{
    if (g_current_mode == APP_MODE_NONE) {
        return;
    } else {
        photo_slideshow.stop();
    }
}

static void switch_app_mode(AppMode target_mode)
{
    if (g_current_mode == target_mode) return;

    stop_current_mode();
    g_current_mode = target_mode;

    // LOCAL mode no longer disconnects STA: we want the web portal reachable
    // over LAN. If we ever re-add a cloud-style mode that needs an explicit
    // STA bring-up, branch on mode_requires_sta() again here.
    if (mode_requires_sta(g_current_mode)) {
        esp_err_t err = ensure_sta_connected_if_needed(false);
        if (err != ESP_OK) {
            ESP_LOGW(g_tag, "Failed to bring up STA: %s", esp_err_to_name(err));
        }
    }

    if (g_current_mode == APP_MODE_LOCAL) {
        photo_slideshow.init("/data", hal.settings.auto_slideshow ? (uint8_t)hal.settings.interval_minutes : 0);
    }

    start_current_mode();
    apply_current_mode_setting();
}

// ---- Pause WiFi auto-reconnect (used during scanning/provisioning) ----

void app_manager_pause_wifi_reconnect(void)
{
    g_wifi_reconnect_paused = true;
}

void app_manager_resume_wifi_reconnect(void)
{
    g_wifi_reconnect_paused = false;
}

esp_err_t app_manager_disconnect_sta_keep_ap(void)
{
    return disconnect_sta_keep_ap_internal();
}

// ---- Mode interface (web calls) ----
esp_err_t app_manager_apply_mode(const char* mode_id)
{
    AppMode target = app_mode_from_mode_id(mode_id);
    if (g_current_mode == target) {
        apply_current_mode_setting();
        return ESP_OK;
    }
    switch_app_mode(target);
    return ESP_OK;
}

esp_err_t app_manager_set_current_mode(const char* mode_id)
{
    char normalized[sizeof(hal.settings.current_mode)] = {0};
    normalize_mode_id(mode_id, normalized, sizeof(normalized));
    if (!is_supported_mode_id(normalized) || cstring_compare(mode_id, normalized) != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    cstring_copy(hal.settings.current_mode, normalized, sizeof(hal.settings.current_mode));
    hal.settingsSave(SETTING_CURRENT_MODE);

    // also update app_server's copy
    app_server_sync_mode(normalized);

    return ESP_OK;
}

// ---- WiFi QR code display ----
static void wifi_qrcode_display_cb(esp_qrcode_handle_t qrcode)
{
    int size  = esp_qrcode_get_size(qrcode);
    int scale = 4;
    if (scale < 1) scale = 1;

    int qr_px    = scale * size;
    int offset_x = 196 - qr_px / 2;
    int offset_y = 100 - qr_px / 2;

    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            if (esp_qrcode_get_module(qrcode, x, y)) {
                hal.Canvas->fillRect(offset_x + x * scale, offset_y + y * scale, scale, scale, TFT_BLACK);
            }
        }
    }

    hal.statusEventSend(OPERATION_EVENT_REFRESH_START);
    hal.Canvas->pushSprite(0, 0);
    hal.statusEventSend(OPERATION_EVENT_REFRESH_COMPLETE);
}

static void show_wifi_config_qrcode(const char* ap_ssid)
{
    uint8_t prev_rotation = hal.Canvas->getRotation();
    hal.Canvas->setRotation(1);

    int img_w = 0;
    int img_h = 0;
    int scr_w = hal.Canvas->width();
    int scr_h = hal.Canvas->height();

    M5.Display.setEpdMode(epd_mode_t::epd_fastest);

#if APP_ASSETS_USE_EMBEDDED
    const uint8_t* bg_data = _binary_PaperColor_png_start;
    size_t bg_len          = (size_t)(_binary_PaperColor_png_end - _binary_PaperColor_png_start);
    if (get_image_size_from_memory(bg_data, bg_len, &img_w, &img_h, ".png")) {
        float s    = std::min((float)scr_w / img_w, (float)scr_h / img_h);
        int draw_x = (scr_w - (int)(img_w * s)) / 2;
        int draw_y = (scr_h - (int)(img_h * s)) / 2;
        hal.Canvas->fillScreen(TFT_WHITE);
        hal.Canvas->drawPng(bg_data, bg_len, draw_x, draw_y, 0, 0, 0, 0, s, s);
    } else {
        hal.Canvas->fillScreen(TFT_WHITE);
    }
#else
    const char* bg_path = "/data/PaperColor.png";
    hal.Canvas->fillScreen(TFT_WHITE);
    hal_storage_prepare_photo_fs_access();
    hal_storage_lock();
    if (get_image_size_from_file(bg_path, &img_w, &img_h)) {
        float s    = std::min((float)scr_w / img_w, (float)scr_h / img_h);
        int draw_x = (scr_w - (int)(img_w * s)) / 2;
        int draw_y = (scr_h - (int)(img_h * s)) / 2;
        hal.Canvas->drawPngFile(bg_path, draw_x, draw_y, 0, 0, 0, 0, s, s);
    }
    hal_storage_unlock();
#endif

    // Clear the QR code area
    hal.Canvas->fillRect(142, 46, 108, 108, TFT_WHITE);

    // Generate WiFi QR code
    char wifi_url[128];
    snprintf(wifi_url, sizeof(wifi_url), "WIFI:S:%s;T:nopass;;", ap_ssid);

    esp_qrcode_config_t cfg = ESP_QRCODE_CONFIG_DEFAULT();
    cfg.display_func        = wifi_qrcode_display_cb;
    cfg.max_qrcode_version  = 10;
    cfg.qrcode_ecc_level    = ESP_QRCODE_ECC_LOW;

    esp_err_t ret = esp_qrcode_generate(&cfg, wifi_url);
    if (ret != ESP_OK) {
        ESP_LOGE(g_tag, "WiFi QRCode failed: %d", ret);
    }
    M5.Display.setEpdMode(epd_mode_t::epd_quality);
    hal.Canvas->setRotation(prev_rotation);
}

void display_boot_guide_image()
{
    int img_w = 0;
    int img_h = 0;
    int scr_w = hal.Canvas->width();
    int scr_h = hal.Canvas->height();

#if APP_ASSETS_USE_EMBEDDED
    const uint8_t* data = _binary_arrowClickToPowerOn_png_start;
    size_t len          = (size_t)(_binary_arrowClickToPowerOn_png_end - _binary_arrowClickToPowerOn_png_start);
    if (get_image_size_from_memory(data, len, &img_w, &img_h, ".png")) {
        float s    = std::min((float)scr_w / img_w, (float)scr_h / img_h);
        int draw_x = (scr_w - (int)(img_w * s)) / 2;
        int draw_y = (scr_h - (int)(img_h * s)) / 2;
        hal.Canvas->fillScreen(TFT_WHITE);
        hal.Canvas->drawPng(data, len, draw_x, draw_y, 0, 0, 0, 0, s, s);
    }
#else
    hal.Canvas->fillScreen(TFT_WHITE);
#endif

    hal.Canvas->setTextColor(RED);
    hal.Canvas->setTextDatum(middle_left);
    hal.Canvas->setFont(&fonts::efontJA_24_b);
    hal.Canvas->drawString("Press to ON", 100, 540);

    hal.statusEventSend(OPERATION_EVENT_REFRESH_START);
    hal.Canvas->pushSprite(0, 0);
    hal.statusEventSend(OPERATION_EVENT_REFRESH_COMPLETE);
}

void app_manager_factory_reset_machine()
{
    ESP_LOGI(g_tag, "Factory reset: restoring defaults");

    stop_current_mode();
    scheduled_push::reset_state();

    hal.settingsLock();
    memset(&hal.settings, 0, sizeof(hal.settings));
    hal.settings.rotation             = 1;
    hal.settings.auto_slideshow       = false;
    hal.settings.interval_minutes     = 60;
    hal.settings.boot_sound           = true;
    hal.settings.low_power_mode       = false;
    hal.settings.gateway_enabled      = false;
    hal.settings.gateway_poll_minutes = 15;
    cstring_copy(hal.settings.device_name, "papercolor", sizeof(hal.settings.device_name));
    hal.settingsUnlock();

    hal.settingsSave(SETTING_WIFI_SSID);
    hal.settingsSave(SETTING_WIFI_PASSWORD);
    hal.settingsSave(SETTING_ROTATION);
    hal.settingsSave(SETTING_AUTO_SLIDESHOW);
    hal.settingsSave(SETTING_INTERVAL);
    hal.settingsSave(SETTING_CURRENT_MODE);
    hal.settingsSave(SETTING_BOOT_SOUND);
    hal.settingsSave(SETTING_DEVICE_NAME);
    hal.settingsSave(SETTING_LOW_POWER_MODE);
    hal.settingsSave(SETTING_GATEWAY_URL);
    hal.settingsSave(SETTING_GATEWAY_TOKEN);
    hal.settingsSave(SETTING_GATEWAY_ENABLED);
    hal.settingsSave(SETTING_GATEWAY_POLL_MINUTES);

    g_current_mode = APP_MODE_NONE;
    app_server_sync_mode("");
    hal.Canvas->setRotation(1);
    display_boot_guide_image();

    vTaskDelay(pdMS_TO_TICKS(500));
    hal.pm1.shutdown();
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// ---- Main event loop ----
static void app_task(void* param)
{
    static uint32_t s_led_blink_timer      = 0;
    static bool s_led_state                = false;
    static bool s_voice_operation_active   = false;
    static uint32_t s_voice_led_timer      = 0;
    static bool s_voice_led_on             = false;
    static uint32_t s_voice_feedback_until = 0;
    static bool s_voice_feedback_success   = false;

    // Generate AP name once (based on MAC)
    uint8_t mac[6];
    hal.getDeviceMac(mac);
    char ap_name[32];
    snprintf(ap_name, sizeof(ap_name), "PaperColor-%02X%02X%02X", mac[3], mac[4], mac[5]);

    while (1) {
        hal.update();

        static uint32_t s_last_scheduled_display_check = 0;
        uint32_t now_ms                                = millis_();
        if (scheduled_push::enabled() && !g_refresh_in_progress &&
            g_active_operations.load(std::memory_order_relaxed) == 0 &&
            (s_last_scheduled_display_check == 0 || now_ms - s_last_scheduled_display_check >= 1000)) {
            s_last_scheduled_display_check = now_ms;
            scheduled_push::display_due_cached(photo_slideshow);
        }

        if (should_idle_power_off_in_low_power_mode()) {
            ESP_LOGI(g_tag, "Low-power idle timeout reached, scheduling wake and powering off");
            shutdown_after_low_power_cycle();
        }

        // ==================== Button handling ====================
        bool btn_a = M5.BtnA.isPressed();
        if (btn_a && g_btn_press_start == 0) {
            g_btn_press_start  = millis_();
            g_btn_long_pressed = false;
            s_led_blink_timer  = millis_();
            s_led_state        = false;
        }

        if (!btn_a) {
            if (g_btn_press_start != 0 && !g_btn_long_pressed) {
                M5.Led.setAllColor(0, 0, 0);
                M5.Led.display();
            }
            g_btn_press_start  = 0;
            g_btn_long_pressed = false;
        }

        bool top_c_single                = M5.BtnC.wasSingleClicked();
        bool top_c_double                = M5.BtnC.wasDoubleClicked();
        voice_capture::Event voice_event = voice_capture::update(top_c_single);
        if (voice_event == voice_capture::Event::Started) {
            app_manager_operation_begin();
            s_voice_operation_active = true;
            M5.Led.setBrightness(80);
            M5.Led.setAllColor(0, 80, 255);
            M5.Led.display();
        } else if (voice_event == voice_capture::Event::Saved) {
            if (s_voice_operation_active) app_manager_operation_end();
            s_voice_operation_active = false;
            s_voice_feedback_until   = now_ms + 1000;
            s_voice_feedback_success = true;
            M5.Led.setAllColor(0, 255, 0);
            M5.Led.display();
            audio::play_tone_from_midi(119, 0.08);
            if (WiFi.isConnected() && scheduled_push::enabled()) {
                scheduled_push::sync_now(photo_slideshow);
            }
        } else if (voice_event == voice_capture::Event::Failed) {
            if (s_voice_operation_active) app_manager_operation_end();
            s_voice_operation_active = false;
            s_voice_feedback_until   = now_ms + 1000;
            s_voice_feedback_success = false;
            M5.Led.setAllColor(255, 0, 0);
            M5.Led.display();
            audio::play_tone_from_midi(72, 0.12);
        }
        if (voice_capture::active()) {
            if (now_ms - s_voice_led_timer >= 300) {
                s_voice_led_timer = now_ms;
                s_voice_led_on    = !s_voice_led_on;
            }
            M5.Led.setBrightness(80);
            M5.Led.setAllColor(0, s_voice_led_on ? 80 : 0, s_voice_led_on ? 255 : 0);
            M5.Led.display();
        } else if (static_cast<int32_t>(s_voice_feedback_until - now_ms) > 0) {
            M5.Led.setBrightness(80);
            M5.Led.setAllColor(s_voice_feedback_success ? 0 : 255, s_voice_feedback_success ? 255 : 0, 0);
            M5.Led.display();
        }

        if (top_c_double && !voice_capture::active() && scheduled_push::enabled() &&
            papercolor::short_button_action(papercolor::ButtonId::TopC) == papercolor::ButtonAction::SyncGateway) {
            audio::play_tone_from_midi(119, 0.08);
            app_manager_mark_activity();
            if (!WiFi.isConnected()) {
                ESP_LOGW(g_tag, "Top C press: gateway synchronization skipped while WiFi is offline");
                hal.statusEventSend(OPERATION_EVENT_FAILED);
            } else {
                ESP_LOGI(g_tag, "Top C press: synchronizing PaperColor gateway");
                hal.statusEventSend(OPERATION_EVENT_REFRESH_START);
                bool sync_ok = scheduled_push::sync_now(photo_slideshow);
                hal.statusEventSend(sync_ok ? OPERATION_EVENT_REFRESH_COMPLETE : OPERATION_EVENT_FAILED);
            }
        }

        if (btn_a && !g_btn_long_pressed) {
            // White LED blink every 200 ms
            if (millis_() - s_led_blink_timer >= 200) {
                s_led_blink_timer = millis_();
                s_led_state       = !s_led_state;
                if (s_led_state) {
                    M5.Led.setBrightness(100);
                    M5.Led.setAllColor(255, 255, 255);
                } else {
                    M5.Led.setAllColor(0, 0, 0);
                }
                M5.Led.display();
            }

            // Long press for 5s: enable WiFi AP
            if (millis_() - g_btn_press_start >= 5000) {
                g_btn_long_pressed = true;

                // Green light confirmation
                M5.Led.setAllColor(0, 255, 0);
                M5.Led.display();

                // Prompt tone
                audio::play_tone_from_midi(100, 0.08);
                vTaskDelay(pdMS_TO_TICKS(80));
                audio::play_tone_from_midi(119, 0.08);

                vTaskDelay(pdMS_TO_TICKS(500));
                M5.Led.setAllColor(0, 0, 0);
                M5.Led.display();

                // Enable AP
                esp_err_t err = ensure_apsta_started();
                if (err != ESP_OK) {
                    ESP_LOGW(g_tag, "Failed to re-enable AP by long press: %s", esp_err_to_name(err));
                } else {
                    ESP_LOGI(g_tag, "AP re-enabled by long press");
                    show_wifi_config_qrcode(ap_name);
                }
            }
        }

        // ==================== WiFi AP auto-off ====================
#if WIFI_AP_AUTO_OFF_ENABLE
        {
            WiFiMode current_wifi_mode = WiFi.getMode();
            bool ap_active             = (current_wifi_mode == WiFiMode::APSTA || current_wifi_mode == WiFiMode::AP);
            bool sta_connected         = WiFi.isConnected();
            bool has_ap_clients        = WiFi.softAPgetStationNum() > 0;

            if (ap_active && !has_ap_clients && sta_connected) {
                if (!g_ap_auto_off_timer_running) {
                    g_ap_auto_off_timer         = millis_();
                    g_ap_auto_off_timer_running = true;
                    ESP_LOGI(g_tag, "AP auto-off timer started (%d min)", WIFI_AP_AUTO_OFF_TIMEOUT_MIN);
                }

                uint32_t elapsed_ms = millis_() - g_ap_auto_off_timer;
                uint32_t timeout_ms = (uint32_t)WIFI_AP_AUTO_OFF_TIMEOUT_MIN * 60000UL;
                if (elapsed_ms >= timeout_ms) {
                    WiFi.softAPdisconnect();
                    g_ap_auto_off_timer_running = false;
                    ESP_LOGI(g_tag, "AP auto-off: closed after %d min", WIFI_AP_AUTO_OFF_TIMEOUT_MIN);
                }
            } else {
                g_ap_auto_off_timer_running = false;
            }
        }

#endif

        if (scheduled_push::periodic_sync_due(millis_()) && WiFi.isConnected() && !g_refresh_in_progress) {
            scheduled_push::sync_now(photo_slideshow);
        }

        // ==================== WiFi status LED refresh + auto-reconnect ====================
        {
            static uint32_t s_last_wifi_check = 0;
            if (millis_() - s_last_wifi_check >= 3000) {
                s_last_wifi_check = millis_();
                if (mode_requires_sta(g_current_mode) && hal.settings.wifi_ssid[0] && !WiFi.isConnected()) {
                    hal.statusEventSend(OPERATION_EVENT_WAITING_WIFI);
                }
            }

            if (mode_requires_sta(g_current_mode) && hal.settings.wifi_ssid[0] && !WiFi.isConnected() &&
                !g_wifi_reconnect_paused) {
                uint32_t now = millis_();
                if (g_wifi_reconnect_last_try_ms == 0 ||
                    now - g_wifi_reconnect_last_try_ms >= WIFI_RECONNECT_INTERVAL_MS) {
                    g_wifi_reconnect_last_try_ms = now;
                    ESP_LOGI(g_tag, "WiFi disconnected, retrying STA connect to %s", hal.settings.wifi_ssid);
                    WiFi.connect(hal.settings.wifi_ssid, hal.settings.wifi_password, 0);
                }
            } else {
                g_wifi_reconnect_last_try_ms = 0;
            }
        }

        // ==================== Dual-mode update ====================
        AppMode active = g_current_mode;
        if (active != APP_MODE_NONE) {
            photo_slideshow.update();
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ---- Startup ----
esp_err_t app_manager_start()
{
    ESP_LOGI(g_tag, "Software version: V%s", APP_SW_VERSION);

    // ── First boot guide image ──
    nvs_handle_t h;
    uint8_t first_boot_done = 0;
    if (nvs_open("papercolor", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "first_boot_done", &first_boot_done);
        nvs_close(h);
    }

    if (!first_boot_done) {
        ESP_LOGI(g_tag, "Show the factory boot guide image at first boot.");
        display_boot_guide_image();

        struct tm factory_tm = {};
        factory_tm.tm_year   = 2026 - 1900;
        factory_tm.tm_mon    = 0;
        factory_tm.tm_mday   = 1;
        factory_tm.tm_hour   = 0;
        factory_tm.tm_min    = 0;
        factory_tm.tm_sec    = 0;
        mktime(&factory_tm);
        M5.Rtc.setDateTime(&factory_tm);

        if (nvs_open("papercolor", NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_u8(h, "first_boot_done", 1);
            nvs_commit(h);
            nvs_close(h);
        }

        hal.pm1.shutdown();

        while (1) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    g_current_mode = app_mode_from_mode_id(hal.settings.current_mode);

    // Coerce stale stored modes from factory firmware back to LOCAL so the
    // device boots into the only remaining mode.
    if (g_current_mode == APP_MODE_NONE && hal.settings.current_mode[0]) {
        g_current_mode = APP_MODE_LOCAL;
        cstring_copy(hal.settings.current_mode, MODE_ID_LOCAL, sizeof(hal.settings.current_mode));
        hal.settingsSave(SETTING_CURRENT_MODE);
    }

    if (!scheduled_push::enabled() && run_rtc_wake_one_shot_cycle()) {
        return ESP_OK;
    }

    if (g_current_mode == APP_MODE_LOCAL) {
        photo_slideshow.init("/data", hal.settings.auto_slideshow ? (uint8_t)hal.settings.interval_minutes : 0);
    }

    if (scheduled_push::enabled()) {
        scheduled_push::display_due_cached(photo_slideshow);
    }

    ESP_ERROR_CHECK(WiFi.begin());

    // STA-first boot policy: if WiFi credentials are saved, try to come up as
    // STA only. Don't broadcast the (open) softAP unless we actually need it
    // for onboarding (no creds saved, or STA connect failed). This avoids the
    // 60–90s window of open AP every boot that the previous "always APSTA, drop
    // later via auto-off" pattern exposed.
    bool need_ap = true;
    if (hal.settings.wifi_ssid[0]) {
        esp_err_t err = WiFi.setMode(WiFiMode::STA);
        if (err != ESP_OK) {
            ESP_LOGW(g_tag, "WiFi.setMode(STA) failed: %s", esp_err_to_name(err));
        } else {
            err = WiFi.connect(hal.settings.wifi_ssid, hal.settings.wifi_password, 15000);
            if (err == ESP_OK) {
                ESP_LOGI(g_tag, "STA connected, IP=%s — skipping AP bring-up", WiFi.localIP().c_str());
                need_ap = false;
            } else {
                ESP_LOGW(g_tag, "STA connect failed (%s)", esp_err_to_name(err));
                hal.statusEventSend(OPERATION_EVENT_WAITING_WIFI);
                if (scheduled_push::enabled()) {
                    need_ap = false;
                    ESP_LOGI(g_tag, "Scheduled mode is offline; preserving the display and retrying later");
                } else {
                    ESP_LOGI(g_tag, "Falling back to AP for re-onboarding");
                }
            }
        }
    }

    if (need_ap) {
        ESP_ERROR_CHECK(ensure_apsta_started());
    }

    // QR code is the re-onboarding affordance shown on the e-paper. We only
    // show it when the AP is actually up and intended for setup (no creds, or
    // STA failed). Once configured and STA connects, the device renders the
    // photo album instead.
    bool need_qrcode = need_ap;

    if (need_qrcode) {
        char ap_name[32];
        get_ap_name(ap_name, sizeof(ap_name));
        show_wifi_config_qrcode(ap_name);
    }

    if (!need_ap && scheduled_push::enabled() && WiFi.isConnected()) {
        scheduled_push::sync_now(photo_slideshow);
    }

    start_current_mode();

    ESP_LOGI(g_tag, "Default mode: %s", mode_id_from_app_mode(g_current_mode));

    if (!need_ap && scheduled_push::enabled() && !hal.externalPowerPresent() && !M5.BtnA.isPressed()) {
        shutdown_after_low_power_cycle();
    }

    xTaskCreate(app_task, "app_mgr", 10240, NULL, 5, NULL);
    return ESP_OK;
}
