/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "ezdata_photo_push.h"
#include <cstring>
#include <cctype>
#include <algorithm>
#include "esp_log.h"
#include "esp_log_level.h"
#include <esp_timer.h>
#include <M5Unified.h>
#include "hal/storage/hal_storage.h"
#include "hal/hal.h"
#include "hal/utils/audio/audio.h"
#include "hal/ezdata/hal_ezdata.h"
#include "hal/utils/image/image_utils.h"
#include "freertos/task.h"
#include "qrcode.h"
#include "apps/app_manager/app_manager.h"

static const char *TAG = "EzdataSlideshow";

#define NO_PHOTO 0xFFFF

static const char *s_current_photo_url = nullptr;
static const char *s_pending_photo_url = nullptr;

static void set_tracked_current_photo_url(const char *url)
{
    s_current_photo_url = url;
}

static void set_tracked_pending_photo_url(const char *url)
{
    s_pending_photo_url = url;
}

static int find_photo_index_by_url_in_list(const char *url)
{
    if (!url || !url[0]) return -1;
    for (size_t photo_index = 0; photo_index < ezdata_get_photo_count(); ++photo_index) {
        const char *item_url = ezdata_get_photo_url(photo_index);
        if (item_url && __builtin_strcmp(item_url, url) == 0) {
            return static_cast<int>(photo_index);
        }
    }
    return -1;
}

static inline uint32_t millis_()
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static void binding_qrcode_display_cb(esp_qrcode_handle_t qrcode)
{
    int size  = esp_qrcode_get_size(qrcode);
    int area  = std::min(hal.Canvas->width(), hal.Canvas->height()) * 0.75f;
    int scale = area / size;
    if (scale < 1) scale = 1;

    int qr_px    = scale * size;
    int offset_x = (hal.Canvas->width() - qr_px) / 2;
    int offset_y = (hal.Canvas->height() - qr_px) / 2;

    hal.Canvas->fillScreen(TFT_WHITE);

    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            if (esp_qrcode_get_module(qrcode, x, y)) {
                hal.Canvas->fillRect(offset_x + x * scale, offset_y + y * scale, scale, scale, TFT_BLACK);
            }
        }
    }

    hal.Canvas->setTextColor(TFT_BLACK, TFT_WHITE);

    // Title close to the top of the QR code
    hal.Canvas->setTextDatum(bottom_center);
    hal.Canvas->setFont(&fonts::Font4);
    hal.Canvas->drawString("Open Device EZData Link", hal.Canvas->width() / 2, offset_y - 8);

    // Hint text close to the bottom of the QR code
    hal.Canvas->setTextDatum(top_center);
    hal.Canvas->setFont(&fonts::Font2);
    int text_y = offset_y + qr_px + 8;
    hal.Canvas->drawString("Scan QR Code to Access", hal.Canvas->width() / 2, text_y);
    std::string token_label = "Token: " + hal.device_token;
    hal.Canvas->drawString(token_label.c_str(), hal.Canvas->width() / 2, text_y + 22);

    hal.statusEventSend(OPERATION_EVENT_REFRESH_START);
    hal.Canvas->pushSprite(0, 0);
    hal.statusEventSend(OPERATION_EVENT_REFRESH_COMPLETE);
}

static void drawBindingQrcode()
{
    if (hal.device_token.empty()) {
        ESP_LOGW(TAG, "device_token not ready, skip qrcode");
        return;
    }

    char qr_url[256];

    snprintf(qr_url, sizeof(qr_url), "https://EzData-PaperColor.m5stack.com/%s/login", hal.device_token.c_str());

    ESP_LOGI(TAG, "Binding URL: %s", qr_url);

    esp_qrcode_config_t cfg = ESP_QRCODE_CONFIG_DEFAULT();
    cfg.display_func        = binding_qrcode_display_cb;
    cfg.max_qrcode_version  = 10;
    cfg.qrcode_ecc_level    = ESP_QRCODE_ECC_MED;

    esp_err_t ret = esp_qrcode_generate(&cfg, qr_url);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "QRCode gen failed: %d", ret);
        hal.Canvas->fillScreen(TFT_WHITE);
        hal.Canvas->setTextColor(TFT_RED);
        hal.Canvas->setCursor(10, 10);
        hal.Canvas->printf("QRCode failed");
        hal.Canvas->pushSprite(0, 0);
    }
}

bool EzdataPhotoPush::init(uint8_t interval_min)
{
    _interval_min                   = interval_min;
    _current_index                  = NO_PHOTO;
    _pending_index                  = NO_PHOTO;
    _running                        = false;
    _needs_refresh                  = false;
    _force_refresh_for_image_update = false;
    _last_btn_c                     = false;
    _last_btn_b                     = false;
    _last_btn_a                     = false;
    _scr_w                          = hal.Canvas->width();
    _scr_h                          = hal.Canvas->height();

    M5.Speaker.setVolume(120);

    ezdata_init();

    hal.statusEventSend(OPERATION_EVENT_STARTUP_SUCCESS);

    ESP_LOGI(TAG, "Init: Ezdata mode, %s%s", interval_min == 0 ? "manual" : "auto",
             interval_min > 0 ? (", interval: " + std::to_string(interval_min) + " min").c_str() : "");
    return true;
}

void EzdataPhotoPush::deinit()
{
    stop();
    _current_index = NO_PHOTO;
    _pending_index = NO_PHOTO;
    set_tracked_current_photo_url(nullptr);
    set_tracked_pending_photo_url(nullptr);
    ezdata_deinit();
}

void EzdataPhotoPush::start()
{
    _running                        = true;
    _current_index                  = NO_PHOTO;
    _pending_index                  = NO_PHOTO;
    _needs_refresh                  = false;
    _force_refresh_for_image_update = false;
    _last_refresh_ms                = millis_();
    _binding_qrcode_scheduled       = false;
    _binding_qrcode_shown_once      = false;
    _startup_state_initialized      = false;
    set_tracked_current_photo_url(nullptr);
    set_tracked_pending_photo_url(nullptr);

    ESP_LOGI(TAG, "Ezdata slideshow started without blocking startup");
}

void EzdataPhotoPush::stop()
{
    _running                        = false;
    _needs_refresh                  = false;
    _force_refresh_for_image_update = false;
    _binding_qrcode_scheduled       = false;
}

void EzdataPhotoPush::next()
{
    if (ezdata_get_photo_count() == 0 || !ezdata_is_connected()) {
        hal.statusEventSend(OPERATION_EVENT_FAILED);
        return;
    }
    hal.statusEventSend(OPERATION_EVENT_SUCCESS);

    uint16_t photo_count = (uint16_t)ezdata_get_photo_count();
    uint16_t new_index   = 0;
    if (_pending_index == NO_PHOTO) {
        new_index = 0;
    } else {
        new_index = (_pending_index + 1) % photo_count;
    }
    requestRefresh(new_index);
}

void EzdataPhotoPush::prev()
{
    if (ezdata_get_photo_count() == 0 || !ezdata_is_connected()) {
        hal.statusEventSend(OPERATION_EVENT_FAILED);
        return;
    }
    hal.statusEventSend(OPERATION_EVENT_SUCCESS);

    uint16_t photo_count = (uint16_t)ezdata_get_photo_count();
    uint16_t new_index   = 0;
    if (_pending_index == NO_PHOTO) {
        new_index = 0;
    } else {
        new_index = (_pending_index == 0) ? (photo_count - 1) : (_pending_index - 1);
    }
    requestRefresh(new_index);
}

void EzdataPhotoPush::goTo(uint16_t index)
{
    if (ezdata_get_photo_count() == 0) return;
    uint16_t photo_count = (uint16_t)ezdata_get_photo_count();
    index                = index % photo_count;
    requestRefresh(index);
}

void EzdataPhotoPush::setInterval(uint8_t min)
{
    _interval_min = min;
}

void EzdataPhotoPush::setSettleTime(uint32_t ms)
{
    _settle_ms = ms;
}

bool EzdataPhotoPush::runOneShotRefresh()
{
    if (ezdata_get_photo_count() == 0) {
        ESP_LOGW(TAG, "One-shot ezdata refresh skipped because no photos were found");
        return false;
    }

    if (!ezdata_is_connected()) {
        ESP_LOGW(TAG, "One-shot ezdata refresh skipped because the device is offline");
        return false;
    }

    if (!ezdata_has_image_record()) {
        ESP_LOGW(TAG, "One-shot ezdata refresh skipped because image record is unavailable");
        return false;
    }

    if (!ezdata_take_image_update_flag()) {
        ESP_LOGI(TAG, "One-shot ezdata refresh skipped because image record is unchanged");
        return false;
    }

    const char *latest_url = ezdata_get_image_record_url();
    if (!latest_url || !latest_url[0]) {
        ESP_LOGW(TAG, "One-shot ezdata refresh skipped because image record URL is empty");
        return false;
    }

    int matched_index = findPhotoIndexByUrl(latest_url);
    if (matched_index < 0) {
        ESP_LOGW(TAG, "One-shot image record URL was not found in list: %s", latest_url);
        return false;
    }

    _pending_index = static_cast<uint16_t>(matched_index);
    clampIndices();

    uint16_t photo_count = (uint16_t)ezdata_get_photo_count();
    uint16_t next_index  = (_pending_index == NO_PHOTO) ? 0 : _pending_index;
    ESP_LOGI(TAG, "One-shot → ezdata [%d/%d]", next_index + 1, (int)photo_count);
    displayPhoto(next_index);
    _current_index = next_index;
    _pending_index = next_index;
    set_tracked_current_photo_url(ezdata_get_photo_url(next_index));
    set_tracked_pending_photo_url(ezdata_get_photo_url(next_index));
    _needs_refresh   = false;
    _last_refresh_ms = millis_();
    hal.rx8130RamWrite(RX8130_RAM_INDEX_EZDATA_CURRENT, (uint8_t)_pending_index);
    uint8_t latest_fingerprint = ezdata_get_image_record_fingerprint();
    hal.rx8130RamWrite(RX8130_RAM_INDEX_EZDATA_FINGERPRINT, latest_fingerprint);
    ezdata_mark_image_record_persisted();
    return true;
}

void EzdataPhotoPush::toggleRotation()
{
    if (ezdata_get_photo_count() == 0 || !ezdata_is_connected()) {
        hal.statusEventSend(OPERATION_EVENT_FAILED);
        return;
    }
    hal.statusEventSend(OPERATION_EVENT_SUCCESS);

    uint8_t current = hal.Canvas->getRotation();
    uint8_t next    = (current == 0) ? 1 : 0;

    hal.settingsLock();
    hal.settings.rotation = next;
    hal.settingsUnlock();

    hal.Canvas->setRotation(next);

    _scr_w = hal.Canvas->width();
    _scr_h = hal.Canvas->height();

    ESP_LOGI(TAG, "Toggle rotation to %d", next);
    ESP_LOGI(TAG, "Toggle screen size to %d, %d", _scr_w, _scr_h);

    syncTrackedIndicesWithList();
    if (_pending_index != NO_PHOTO) {
        displayPhoto(_pending_index);
    }
}

void EzdataPhotoPush::syncSettings()
{
    hal.settingsLock();
    uint8_t target_interval =
        (hal.settings.auto_slideshow && hal.settings.interval_minutes > 0) ? (uint8_t)hal.settings.interval_minutes : 0;
    uint8_t target_rotation = hal.settings.rotation;
    hal.settingsUnlock();

    if (_interval_min != target_interval) {
        _interval_min = target_interval;
    }

    if (hal.Canvas->getRotation() != target_rotation) {
        hal.Canvas->setRotation(target_rotation);
        _scr_w = hal.Canvas->width();
        _scr_h = hal.Canvas->height();
    }
}

void EzdataPhotoPush::update()
{
    if (!_running) return;

    syncSettings();
    handleButtons();

    if (!_startup_state_initialized) {
        if (ezdata_is_connected()) {
            if (!hal.device_token.empty()) {
                _binding_qrcode_scheduled = true;
            }
            ESP_LOGI(TAG, "Ezdata connected after startup");
        }

        if (ezdata_get_photo_count() != 0) {
            uint8_t stored_index = 0;
            hal.rx8130RamRead(RX8130_RAM_INDEX_EZDATA_CURRENT, &stored_index);
            _pending_index = stored_index;
            clampIndices();
            uint16_t photo_count = (uint16_t)ezdata_get_photo_count();
            if (_pending_index != NO_PHOTO && _pending_index < photo_count) {
                set_tracked_pending_photo_url(ezdata_get_photo_url(_pending_index));
            }
            _startup_state_initialized = true;
        } else if (!ezdata_is_connected()) {
            if (!hal.device_token.empty()) {
                _binding_qrcode_scheduled = true;
            }
            ESP_LOGW(TAG, "Startup waiting for WiFi/Ezdata without blocking");
            _startup_state_initialized = true;
        }
    }

    // Startup scene: after the daemon starts it may take a few seconds to connect, and _startup_state_initialized is
    // already set. We need to set _binding_qrcode_scheduled so the QR code will still be drawn.
    if (_startup_state_initialized && !_binding_qrcode_shown_once && !_binding_qrcode_scheduled) {
        if (ezdata_is_connected() && !hal.device_token.empty()) {
            _binding_qrcode_scheduled = true;
        }
    }

    if (_binding_qrcode_scheduled && !_binding_qrcode_shown_once && hal.device_token.size() > 0) {
        drawBindingQrcode();
        _binding_qrcode_shown_once = true;
        _binding_qrcode_scheduled  = false;
        return;
    }

    if (ezdata_has_new_photo()) {
        const char *latest_url = ezdata_get_new_photo_url();
        ESP_LOGI(TAG, "New photo pushed from USER_ADD_DATA");
        if (latest_url && latest_url[0] && getImageFileLinkList()) {
            set_tracked_pending_photo_url(latest_url);
            int matched_index = findPhotoIndexByUrl(latest_url);
            if (matched_index >= 0) {
                _pending_index                  = static_cast<uint16_t>(matched_index);
                _needs_refresh                  = true;
                _force_refresh_for_image_update = true;
                _last_button_ms                 = 0;
            } else {
                ESP_LOGW(TAG, "New photo URL was not found in list: %s", latest_url);
            }
        }
        ezdata_clear_new_photo_flag();
    }

    if (ezdata_take_image_update_flag()) {
        const char *latest_url = ezdata_get_image_record_url();
        ESP_LOGI(TAG, "Image record updated from DEVICE_QUERY_DATA");
        if (latest_url && latest_url[0]) {
            set_tracked_pending_photo_url(latest_url);
            int matched_index = findPhotoIndexByUrl(latest_url);
            if (matched_index >= 0) {
                _pending_index                  = static_cast<uint16_t>(matched_index);
                _needs_refresh                  = true;
                _force_refresh_for_image_update = true;
                _last_button_ms                 = 0;
            } else {
                ESP_LOGW(TAG, "Latest image record not found in list: %s", latest_url);
            }
        }
    }

    if (_needs_refresh) {
        if (millis_() - _last_button_ms >= _settle_ms) {
            if (ezdata_get_photo_count() == 0) {
                _needs_refresh = false;
                ESP_LOGW(TAG, "No ezdata photos, cancel refresh");
                return;
            }
            clampIndices();

            if (!_force_refresh_for_image_update && _pending_index == _current_index && _current_index != NO_PHOTO &&
                ((s_pending_photo_url && s_current_photo_url &&
                  __builtin_strcmp(s_pending_photo_url, s_current_photo_url) == 0) ||
                 (!s_pending_photo_url && !s_current_photo_url))) {
                _needs_refresh = false;
                ESP_LOGI(TAG, "After rescan, pending == current, skip refresh");
                return;
            }

            if (!ezdata_is_connected()) {
                ESP_LOGW(TAG, "Ezdata not connected, skip refresh");
                _needs_refresh = false;
                return;
            }

            ESP_LOGI(TAG, "Settle OK → refreshing ezdata [%d/%d]", _pending_index + 1, (int)ezdata_get_photo_count());
            displayPhoto(_pending_index);
            _current_index = _pending_index;
            if (_current_index != NO_PHOTO && _current_index < ezdata_get_photo_count()) {
                set_tracked_current_photo_url(ezdata_get_photo_url(_current_index));
                set_tracked_pending_photo_url(ezdata_get_photo_url(_current_index));
            }
            _needs_refresh                  = false;
            _force_refresh_for_image_update = false;
            _last_refresh_ms                = millis_();
            hal.rx8130RamWrite(RX8130_RAM_INDEX_EZDATA_CURRENT, (uint8_t)_current_index);
            if (ezdata_has_image_record()) {
                uint8_t latest_fingerprint = ezdata_get_image_record_fingerprint();
                hal.rx8130RamWrite(RX8130_RAM_INDEX_EZDATA_FINGERPRINT, latest_fingerprint);
                ezdata_mark_image_record_persisted();
            }
        }
        return;
    }

    if (_interval_min > 0) {
        uint32_t interval_ms = (uint32_t)_interval_min * 60000UL;
        if (millis_() - _last_refresh_ms >= interval_ms) {
            _last_refresh_ms = millis_();

            if (!ezdata_is_connected()) {
                ESP_LOGW(TAG, "Ezdata not connected, skip auto refresh");
                return;
            }

            if (!getImageFileLinkList()) {
                ESP_LOGW(TAG, "Auto refresh skipped because photo list query failed");
                return;
            }

            if (!ezdata_has_image_record()) {
                ESP_LOGW(TAG, "Auto refresh skipped because image record is unavailable");
                return;
            }

            if (!ezdata_take_image_update_flag()) {
                ESP_LOGD(TAG, "Auto refresh skipped because image record is unchanged");
                return;
            }

            uint8_t latest_fingerprint = ezdata_get_image_record_fingerprint();
            const char *latest_url     = ezdata_get_image_record_url();
            if (!latest_url || !latest_url[0]) {
                ESP_LOGW(TAG, "Auto refresh skipped because image record URL is empty");
                return;
            }

            int matched_index = findPhotoIndexByUrl(latest_url);
            if (matched_index < 0) {
                ESP_LOGW(TAG, "Auto refresh skipped because image URL was not found in list: %s", latest_url);
                return;
            }

            _pending_index = static_cast<uint16_t>(matched_index);
            set_tracked_pending_photo_url(latest_url);

            if (_pending_index == _current_index && _current_index != NO_PHOTO) {
                hal.rx8130RamWrite(RX8130_RAM_INDEX_EZDATA_FINGERPRINT, latest_fingerprint);
                ezdata_mark_image_record_persisted();
                ESP_LOGI(TAG, "Auto refresh only updated fingerprint for current ezdata image");
                return;
            }

            ESP_LOGI(TAG, "Auto refresh → ezdata [%d/%d]", _pending_index + 1, (int)ezdata_get_photo_count());
            displayPhoto(_pending_index);
            _current_index = _pending_index;
            set_tracked_current_photo_url(latest_url);
            set_tracked_pending_photo_url(latest_url);
            hal.rx8130RamWrite(RX8130_RAM_INDEX_EZDATA_CURRENT, (uint8_t)_current_index);
            hal.rx8130RamWrite(RX8130_RAM_INDEX_EZDATA_FINGERPRINT, latest_fingerprint);
            ezdata_mark_image_record_persisted();
        }
    }
}

uint16_t EzdataPhotoPush::getTotal() const
{
    return (uint16_t)ezdata_get_photo_count();
}

uint16_t EzdataPhotoPush::getCurrentIndex() const
{
    return (_current_index == NO_PHOTO) ? 0 : _current_index;
}

uint16_t EzdataPhotoPush::getPendingIndex() const
{
    return (_pending_index == NO_PHOTO) ? 0 : _pending_index;
}

bool EzdataPhotoPush::isRunning() const
{
    return _running;
}

bool EzdataPhotoPush::isWaitingSettle() const
{
    return _needs_refresh;
}

int EzdataPhotoPush::findPhotoIndexByUrl(const char *url) const
{
    return find_photo_index_by_url_in_list(url);
}

void EzdataPhotoPush::syncTrackedIndicesWithList()
{
    uint16_t photo_count = (uint16_t)ezdata_get_photo_count();
    if (photo_count == 0) {
        _current_index = NO_PHOTO;
        _pending_index = NO_PHOTO;
        set_tracked_current_photo_url(nullptr);
        set_tracked_pending_photo_url(nullptr);
        return;
    }

    if (s_current_photo_url && s_current_photo_url[0]) {
        int photo_index = findPhotoIndexByUrl(s_current_photo_url);
        if (photo_index >= 0) {
            _current_index = (uint16_t)photo_index;
        }
    }

    if (s_pending_photo_url && s_pending_photo_url[0]) {
        int photo_index = findPhotoIndexByUrl(s_pending_photo_url);
        if (photo_index >= 0) {
            _pending_index = (uint16_t)photo_index;
        }
    }
}

void EzdataPhotoPush::clampIndices()
{
    uint16_t photo_count = (uint16_t)ezdata_get_photo_count();
    if (photo_count == 0) return;

    syncTrackedIndicesWithList();

    if (_current_index != NO_PHOTO && _current_index >= photo_count) {
        _current_index = _current_index % photo_count;
    }
    if (_pending_index != NO_PHOTO && _pending_index >= photo_count) {
        _pending_index = _pending_index % photo_count;
    }

    if (_current_index != NO_PHOTO && _current_index < photo_count) {
        set_tracked_current_photo_url(ezdata_get_photo_url(_current_index));
    }
    if (_pending_index != NO_PHOTO && _pending_index < photo_count) {
        set_tracked_pending_photo_url(ezdata_get_photo_url(_pending_index));
    }
}

void EzdataPhotoPush::requestRefresh(uint16_t new_index)
{
    _pending_index = new_index;
    if (_pending_index < ezdata_get_photo_count()) {
        set_tracked_pending_photo_url(ezdata_get_photo_url(_pending_index));
    }
    hal.rx8130RamWrite(RX8130_RAM_INDEX_EZDATA_CURRENT, (uint8_t)_pending_index);
    if (_pending_index == _current_index && _current_index != NO_PHOTO) {
        _needs_refresh = false;
        ESP_LOGI(TAG, "Back to current ezdata [%d/%d], cancel refresh", _current_index + 1,
                 (int)ezdata_get_photo_count());
        return;
    }
    _needs_refresh                  = true;
    _force_refresh_for_image_update = false;
    _last_button_ms                 = millis_();
    ESP_LOGI(TAG, "Pending → ezdata [%d/%d]", _pending_index + 1, (int)ezdata_get_photo_count());
}

void EzdataPhotoPush::handleButtons()
{
    bool button_a_pressed  = M5.BtnA.wasPressed();
    bool button_a_released = M5.BtnA.wasClicked();
    bool button_c_pressed  = M5.BtnC.wasPressed();
    bool button_b_pressed  = M5.BtnB.wasPressed();
    if (button_c_pressed) audio::play_tone_from_midi(119, 0.08);
    if (button_b_pressed) audio::play_tone_from_midi(120, 0.08);
    if (button_a_pressed) audio::play_tone_from_midi(121, 0.08);
    if (button_a_released && !_last_btn_a) toggleRotation();
    if (button_c_pressed && !_last_btn_c) prev();
    if (button_b_pressed && !_last_btn_b) next();
    _last_btn_c = button_c_pressed;
    _last_btn_b = button_b_pressed;
    _last_btn_a = button_a_released;
}

void EzdataPhotoPush::displayPhoto(uint16_t index)
{
    int image_width = 0, image_height = 0;
    uint8_t *img_data = nullptr;
    size_t img_len    = 0;

    uint16_t photo_count = (uint16_t)ezdata_get_photo_count();
    if (photo_count == 0 || index >= photo_count) {
        ESP_LOGE(TAG, "Invalid index or empty photo list");
        hal.statusEventSend(OPERATION_EVENT_ERROR_IMAGE_READ);
        return;
    }

    esp_err_t err = ezdata_fetch_photo(index, &img_data, &img_len, MAX_IMAGE_SIZE);
    if (err != ESP_OK || !img_data || img_len == 0) {
        ESP_LOGE(TAG, "Failed to fetch ezdata photo: %s", esp_err_to_name(err));
        hal.statusEventSend(OPERATION_EVENT_ERROR_IMAGE_READ);
        return;
    }

    const char *url = ezdata_get_photo_url(index);
    if (!url) {
        ESP_LOGE(TAG, "Photo URL not found for index %d", index);
        hal.statusEventSend(OPERATION_EVENT_ERROR_IMAGE_READ);
        if (img_data) heap_caps_free(img_data);
        return;
    }
    const char *dot = strrchr(url, '.');
    if (!dot || !get_image_size_from_memory(img_data, img_len, &image_width, &image_height, dot)) {
        ESP_LOGE(TAG, "Failed to read image size from memory");
        hal.statusEventSend(OPERATION_EVENT_ERROR_IMAGE_READ);
        if (img_data) heap_caps_free(img_data);
        return;
    }

    float scale = std::min((float)_scr_w / image_width, (float)_scr_h / image_height);
    int draw_x  = (_scr_w - (int)(image_width * scale)) / 2;
    int draw_y  = (_scr_h - (int)(image_height * scale)) / 2;

    ESP_LOGI(TAG, "Image: %dx%d, scale: %.2f, draw: (%d,%d)", image_width, image_height, scale, draw_x, draw_y);

    hal.Canvas->fillScreen(TFT_WHITE);

    if (dot) {
        if (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0) {
            hal.Canvas->drawJpg(img_data, img_len, draw_x, draw_y, 0, 0, 0, 0, scale, scale);
            hal.statusEventSend(OPERATION_EVENT_REFRESH_START);
            app_manager_set_refresh_in_progress(true);
            hal.Canvas->pushSprite(0, 0);
            app_manager_set_refresh_in_progress(false);
            hal.statusEventSend(OPERATION_EVENT_REFRESH_COMPLETE);
        } else if (strcasecmp(dot, ".bmp") == 0) {
            hal.Canvas->drawBmp(img_data, img_len, draw_x, draw_y, 0, 0, 0, 0, scale, scale);
            hal.statusEventSend(OPERATION_EVENT_REFRESH_START);
            app_manager_set_refresh_in_progress(true);
            hal.Canvas->pushSprite(0, 0);
            app_manager_set_refresh_in_progress(false);
            hal.statusEventSend(OPERATION_EVENT_REFRESH_COMPLETE);
        } else if (strcasecmp(dot, ".png") == 0) {
            hal.Canvas->drawPng(img_data, img_len, draw_x, draw_y, 0, 0, 0, 0, scale, scale);
            hal.statusEventSend(OPERATION_EVENT_REFRESH_START);
            app_manager_set_refresh_in_progress(true);
            hal.Canvas->pushSprite(0, 0);
            app_manager_set_refresh_in_progress(false);
            hal.statusEventSend(OPERATION_EVENT_REFRESH_COMPLETE);
        }
    }

    if (img_data) {
        heap_caps_free(img_data);
    }
}
