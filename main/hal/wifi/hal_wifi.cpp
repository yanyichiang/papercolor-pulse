/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal_wifi.h"

#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/task.h"
#include "nvs_flash.h"

namespace hal_wifi {

static const char* TAG = "WiFiMgr";

// Event group bit definitions
static constexpr int BIT_CONNECTED = BIT0;
static constexpr int BIT_FAIL      = BIT1;
static constexpr int BIT_GOT_IP    = BIT2;
static constexpr int BIT_SCAN_DONE = BIT3;

// Global instance
WiFiManager WiFi;

WiFiManager::WiFiManager()
{
}
WiFiManager::~WiFiManager()
{
    end();
}

// --------------- Initialization ---------------
esp_err_t WiFiManager::begin()
{
    if (_initialized) return ESP_OK;

    // 1. NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. netif + event loop
    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }

    // 3. Create the default AP and STA netifs for later use by mode
    _sta_netif = esp_netif_create_default_wifi_sta();
    _ap_netif  = esp_netif_create_default_wifi_ap();

    // 4. Initialize WiFi
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // 5. Event group
    _event_group = xEventGroupCreate();

    // 6. Register events
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &WiFiManager::eventHandlerStatic,
                                                        this, &_wifi_handler_inst));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, &WiFiManager::eventHandlerStatic,
                                                        this, &_ip_handler_inst));

    // 7. Use RAM storage mode to avoid restart leftovers (optional)
    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    _initialized = true;
    ESP_LOGI(TAG, "WiFiManager initialized");
    return ESP_OK;
}

esp_err_t WiFiManager::end()
{
    if (!_initialized) return ESP_OK;

    esp_wifi_stop();
    esp_wifi_deinit();

    if (_wifi_handler_inst) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, _wifi_handler_inst);
        _wifi_handler_inst = nullptr;
    }
    if (_ip_handler_inst) {
        esp_event_handler_instance_unregister(IP_EVENT, ESP_EVENT_ANY_ID, _ip_handler_inst);
        _ip_handler_inst = nullptr;
    }

    if (_sta_netif) {
        esp_netif_destroy_default_wifi(_sta_netif);
        _sta_netif = nullptr;
    }
    if (_ap_netif) {
        esp_netif_destroy_default_wifi(_ap_netif);
        _ap_netif = nullptr;
    }

    if (_event_group) {
        vEventGroupDelete(_event_group);
        _event_group = nullptr;
    }

    _mode        = WiFiMode::OFF;
    _initialized = false;
    return ESP_OK;
}

// --------------- Events ---------------
void WiFiManager::eventHandlerStatic(void* arg, esp_event_base_t base, int32_t id, void* data)
{
    static_cast<WiFiManager*>(arg)->eventHandler(base, id, data);
}

void WiFiManager::eventHandler(esp_event_base_t base, int32_t id, void* data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGD(TAG, "STA_START");
                fireEvent(WiFiEvent::STA_START, data);
                break;
            case WIFI_EVENT_STA_STOP:
                ESP_LOGD(TAG, "STA_STOP");
                fireEvent(WiFiEvent::STA_STOP, data);
                break;
            case WIFI_EVENT_STA_CONNECTED:
                ESP_LOGD(TAG, "STA_CONNECTED");
                _sta_connected = true;
                xEventGroupSetBits(_event_group, BIT_CONNECTED);
                fireEvent(WiFiEvent::STA_CONNECTED, data);
                break;
            case WIFI_EVENT_STA_DISCONNECTED: {
                ESP_LOGD(TAG, "STA_DISCONNECTED");
                _sta_connected = false;
                _sta_got_ip    = false;
                xEventGroupSetBits(_event_group, BIT_FAIL);
                fireEvent(WiFiEvent::STA_DISCONNECTED, data);
                break;
            }
            case WIFI_EVENT_AP_START:
                ESP_LOGD(TAG, "AP_START");
                fireEvent(WiFiEvent::AP_START, data);
                break;
            case WIFI_EVENT_AP_STOP:
                ESP_LOGD(TAG, "AP_STOP");
                fireEvent(WiFiEvent::AP_STOP, data);
                break;
            case WIFI_EVENT_AP_STACONNECTED: {
                auto* e = static_cast<wifi_event_ap_staconnected_t*>(data);
                ESP_LOGD(TAG, "AP_STA_CONN MAC=" MACSTR, MAC2STR(e->mac));
                fireEvent(WiFiEvent::AP_STA_CONNECTED, data);
                break;
            }
            case WIFI_EVENT_AP_STADISCONNECTED: {
                auto* e = static_cast<wifi_event_ap_stadisconnected_t*>(data);
                ESP_LOGD(TAG, "AP_STA_DISC MAC=" MACSTR, MAC2STR(e->mac));
                fireEvent(WiFiEvent::AP_STA_DISCONNECTED, data);
                break;
            }
            case WIFI_EVENT_SCAN_DONE:
                ESP_LOGD(TAG, "SCAN_DONE");
                xEventGroupSetBits(_event_group, BIT_SCAN_DONE);
                fireEvent(WiFiEvent::SCAN_DONE, data);
                break;
            default:
                break;
        }
    } else if (base == IP_EVENT) {
        switch (id) {
            case IP_EVENT_STA_GOT_IP: {
                auto* e      = static_cast<ip_event_got_ip_t*>(data);
                _sta_ip      = e->ip_info.ip;
                _sta_gw      = e->ip_info.gw;
                _sta_netmask = e->ip_info.netmask;
                _sta_got_ip  = true;
                ESP_LOGI(TAG, "GOT_IP: " IPSTR, IP2STR(&e->ip_info.ip));
                xEventGroupSetBits(_event_group, BIT_GOT_IP);
                fireEvent(WiFiEvent::STA_GOT_IP, data);
                break;
            }
            case IP_EVENT_STA_LOST_IP:
                _sta_got_ip = false;
                fireEvent(WiFiEvent::STA_LOST_IP, data);
                break;
            default:
                break;
        }
    }
}

void WiFiManager::fireEvent(WiFiEvent ev, void* data)
{
    if (_user_cb) _user_cb(ev, data);
}

void WiFiManager::onEvent(EventCallback cb)
{
    _user_cb = std::move(cb);
}

// --------------- Mode ---------------
esp_err_t WiFiManager::setMode(WiFiMode mode)
{
    if (!_initialized) begin();
    wifi_mode_t cur;
    esp_wifi_get_mode(&cur);
    wifi_mode_t target = static_cast<wifi_mode_t>(mode);

    if (cur == target) {
        _mode = mode;
        return ESP_OK;
    }

    // If Wi-Fi is already running, stop it first, change the mode, then start it again
    bool was_started = (cur != WIFI_MODE_NULL);
    if (was_started) esp_wifi_stop();

    esp_err_t err = esp_wifi_set_mode(target);
    if (err != ESP_OK) return err;

    if (target != WIFI_MODE_NULL) {
        err = esp_wifi_start();
        if (err != ESP_OK) return err;
    }

    _mode = mode;
    return ESP_OK;
}

WiFiMode WiFiManager::getMode() const
{
    return _mode;
}

// --------------- STA ---------------
esp_err_t WiFiManager::connect(const std::string& ssid, const std::string& password, uint32_t timeout_ms)
{
    if (!_initialized) begin();

    // If currently in AP mode, upgrade to APSTA
    WiFiMode target = _mode;
    if (_mode == WiFiMode::OFF) {
        target = WiFiMode::STA;
    } else if (_mode == WiFiMode::AP) {
        target = WiFiMode::APSTA;
    } else {
        target = _mode;
    }
    ESP_ERROR_CHECK(setMode(target));

    wifi_config_t cfg{};
    std::strncpy(reinterpret_cast<char*>(cfg.sta.ssid), ssid.c_str(), sizeof(cfg.sta.ssid) - 1);
    std::strncpy(reinterpret_cast<char*>(cfg.sta.password), password.c_str(), sizeof(cfg.sta.password) - 1);
    cfg.sta.threshold.authmode = password.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    cfg.sta.pmf_cfg.capable    = true;
    cfg.sta.pmf_cfg.required   = false;

    _configured_ssid     = ssid;
    _configured_password = password;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));

    xEventGroupClearBits(_event_group, BIT_CONNECTED | BIT_FAIL | BIT_GOT_IP);

    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_connect fail: %s", esp_err_to_name(err));
        return err;
    }

    if (timeout_ms == 0) return ESP_OK;  // Asynchronous

    EventBits_t bits =
        xEventGroupWaitBits(_event_group, BIT_GOT_IP | BIT_FAIL, pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));

    if (bits & BIT_GOT_IP) return ESP_OK;
    if (bits & BIT_FAIL) return ESP_FAIL;
    return ESP_ERR_TIMEOUT;
}

esp_err_t WiFiManager::disconnect()
{
    return esp_wifi_disconnect();
}

bool WiFiManager::isConnected() const
{
    return _sta_connected && _sta_got_ip;
}

static std::string ip4ToString(const esp_ip4_addr_t& ip)
{
    char buf[16];
    snprintf(buf, sizeof(buf), IPSTR, IP2STR(&ip));
    return std::string(buf);
}

std::string WiFiManager::localIP() const
{
    return ip4ToString(_sta_ip);
}
std::string WiFiManager::gatewayIP() const
{
    return ip4ToString(_sta_gw);
}
std::string WiFiManager::subnetMask() const
{
    return ip4ToString(_sta_netmask);
}

std::string WiFiManager::macAddressSTA() const
{
    uint8_t mac[6]{};
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    char buf[18];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return std::string(buf);
}

int8_t WiFiManager::RSSI()
{
    wifi_ap_record_t info{};
    if (esp_wifi_sta_get_ap_info(&info) == ESP_OK) return info.rssi;
    return 0;
}

std::string WiFiManager::SSID()
{
    wifi_ap_record_t info{};
    if (esp_wifi_sta_get_ap_info(&info) == ESP_OK) {
        return reinterpret_cast<const char*>(info.ssid);
    }
    return _configured_ssid;
}

std::string WiFiManager::BSSID()
{
    wifi_ap_record_t info{};
    if (esp_wifi_sta_get_ap_info(&info) == ESP_OK) {
        char buf[18];
        snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", info.bssid[0], info.bssid[1], info.bssid[2],
                 info.bssid[3], info.bssid[4], info.bssid[5]);
        return std::string(buf);
    }
    return "";
}

uint8_t WiFiManager::channel()
{
    wifi_ap_record_t info{};
    if (esp_wifi_sta_get_ap_info(&info) == ESP_OK) {
        return info.primary;
    }
    return 0;
}

wifi_auth_mode_t WiFiManager::authMode()
{
    wifi_ap_record_t info{};
    if (esp_wifi_sta_get_ap_info(&info) == ESP_OK) {
        return info.authmode;
    }
    return WIFI_AUTH_OPEN;
}

// --------------- AP ---------------
esp_err_t WiFiManager::softAP(const std::string& ssid, const std::string& password, uint8_t channel, bool hidden,
                              uint8_t max_connection)
{
    if (!_initialized) begin();

    WiFiMode target = _mode;
    if (_mode == WiFiMode::OFF) {
        target = WiFiMode::AP;
    } else if (_mode == WiFiMode::STA) {
        target = WiFiMode::APSTA;
    } else {
        target = _mode;
    }
    ESP_ERROR_CHECK(setMode(target));

    wifi_config_t cfg{};
    std::strncpy(reinterpret_cast<char*>(cfg.ap.ssid), ssid.c_str(), sizeof(cfg.ap.ssid) - 1);
    cfg.ap.ssid_len         = ssid.length();
    cfg.ap.channel          = channel;
    cfg.ap.max_connection   = max_connection;
    cfg.ap.ssid_hidden      = hidden ? 1 : 0;
    cfg.ap.pmf_cfg.required = false;

    if (password.empty()) {
        cfg.ap.authmode = WIFI_AUTH_OPEN;
    } else {
        std::strncpy(reinterpret_cast<char*>(cfg.ap.password), password.c_str(), sizeof(cfg.ap.password) - 1);
        cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    }

    esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &cfg);
    if (err != ESP_OK) return err;

    err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) return err;

    return ESP_OK;
}

esp_err_t WiFiManager::softAPdisconnect(bool wifioff)
{
    if (_mode == WiFiMode::APSTA) {
        setMode(WiFiMode::STA);
    } else if (_mode == WiFiMode::AP) {
        setMode(wifioff ? WiFiMode::OFF : WiFiMode::OFF);
    }
    return ESP_OK;
}

std::string WiFiManager::softAPIP() const
{
    esp_netif_ip_info_t ip{};
    if (_ap_netif && esp_netif_get_ip_info(_ap_netif, &ip) == ESP_OK) {
        return ip4ToString(ip.ip);
    }
    return "0.0.0.0";
}

std::string WiFiManager::macAddressAP() const
{
    uint8_t mac[6]{};
    esp_wifi_get_mac(WIFI_IF_AP, mac);
    char buf[18];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return std::string(buf);
}

uint8_t WiFiManager::softAPgetStationNum() const
{
    wifi_sta_list_t list{};
    if (esp_wifi_ap_get_sta_list(&list) == ESP_OK) return list.num;
    return 0;
}

// --------------- Scan ---------------
esp_err_t WiFiManager::ensureStaForScan(WiFiMode& restore_mode)
{
    restore_mode = _mode;
    if (_mode == WiFiMode::OFF) return setMode(WiFiMode::STA);
    if (_mode == WiFiMode::AP) return setMode(WiFiMode::APSTA);
    return ESP_OK;
}

esp_err_t WiFiManager::scanNetworks(std::vector<WiFiScanResult>& results, bool show_hidden, uint32_t timeout_ms)
{
    if (!_initialized) begin();

    WiFiMode restore_mode;
    ensureStaForScan(restore_mode);

    wifi_scan_config_t sc{};
    sc.ssid                 = nullptr;
    sc.bssid                = nullptr;
    sc.channel              = 0;
    sc.show_hidden          = show_hidden;
    sc.scan_type            = WIFI_SCAN_TYPE_ACTIVE;
    sc.scan_time.active.min = 100;
    sc.scan_time.active.max = 300;

    xEventGroupClearBits(_event_group, BIT_SCAN_DONE);

    esp_err_t err = esp_wifi_scan_start(&sc, false);  // Asynchronous
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "scan_start fail: %s", esp_err_to_name(err));
        return err;
    }

    EventBits_t bits = xEventGroupWaitBits(_event_group, BIT_SCAN_DONE, pdTRUE, pdFALSE, pdMS_TO_TICKS(timeout_ms));

    if (!(bits & BIT_SCAN_DONE)) {
        esp_wifi_scan_stop();
        return ESP_ERR_TIMEOUT;
    }

    return getScanResults(results);
}

esp_err_t WiFiManager::scanNetworksAsync(bool show_hidden)
{
    if (!_initialized) begin();
    WiFiMode restore_mode;
    ensureStaForScan(restore_mode);

    wifi_scan_config_t sc{};
    sc.show_hidden          = show_hidden;
    sc.scan_type            = WIFI_SCAN_TYPE_ACTIVE;
    sc.scan_time.active.min = 100;
    sc.scan_time.active.max = 300;

    xEventGroupClearBits(_event_group, BIT_SCAN_DONE);
    return esp_wifi_scan_start(&sc, false);
}

esp_err_t WiFiManager::getScanResults(std::vector<WiFiScanResult>& results)
{
    uint16_t num  = 0;
    esp_err_t err = esp_wifi_scan_get_ap_num(&num);
    if (err != ESP_OK) return err;

    if (num == 0) {
        results.clear();
        return ESP_OK;
    }

    std::vector<wifi_ap_record_t> records(num);
    err = esp_wifi_scan_get_ap_records(&num, records.data());
    if (err != ESP_OK) return err;

    results.clear();
    results.reserve(num);
    for (uint16_t i = 0; i < num; ++i) {
        WiFiScanResult r;
        r.ssid = reinterpret_cast<const char*>(records[i].ssid);
        std::memcpy(r.bssid, records[i].bssid, 6);
        r.rssi     = records[i].rssi;
        r.channel  = records[i].primary;
        r.authmode = records[i].authmode;
        results.push_back(std::move(r));
    }
    return ESP_OK;
}

}  // namespace hal_wifi