/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <functional>
#include <string>
#include <vector>
#include <cstdint>

#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

namespace hal_wifi {

/**
 * @brief Wi-Fi scan result.
 */
struct WiFiScanResult {
    std::string ssid;
    uint8_t bssid[6];
    int8_t rssi;
    uint8_t channel;
    wifi_auth_mode_t authmode;
};

/**
 * @brief Supported Wi-Fi operating modes.
 */
enum class WiFiMode {
    OFF   = WIFI_MODE_NULL,
    STA   = WIFI_MODE_STA,
    AP    = WIFI_MODE_AP,
    APSTA = WIFI_MODE_APSTA,
};

/**
 * @brief Event types delivered to user callbacks.
 */
enum class WiFiEvent {
    STA_START,
    STA_STOP,
    STA_CONNECTED,
    STA_DISCONNECTED,
    STA_GOT_IP,
    STA_LOST_IP,
    AP_START,
    AP_STOP,
    AP_STA_CONNECTED,
    AP_STA_DISCONNECTED,
    SCAN_DONE,
};

using EventCallback = std::function<void(WiFiEvent event, void* data)>;

/**
 * @brief Wi-Fi manager wrapper around ESP-IDF networking.
 */
class WiFiManager {
public:
    WiFiManager();
    ~WiFiManager();

    WiFiManager(const WiFiManager&)            = delete;
    WiFiManager& operator=(const WiFiManager&) = delete;

    /** @brief Initializes Wi-Fi, NVS, and networking objects. */
    esp_err_t begin();

    /** @brief Deinitializes Wi-Fi and releases networking objects. */
    esp_err_t end();

    /** @brief Sets the active Wi-Fi mode. */
    esp_err_t setMode(WiFiMode mode);
    /** @brief Returns the current Wi-Fi mode. */
    WiFiMode getMode() const;

    /** @brief Connects to an access point. */
    esp_err_t connect(const std::string& ssid, const std::string& password, uint32_t timeout_ms = 15000);
    /** @brief Disconnects from the current access point. */
    esp_err_t disconnect();
    /** @brief Returns whether STA is connected. */
    bool isConnected() const;
    /** @brief Returns the STA IP address. */
    std::string localIP() const;
    /** @brief Returns the STA gateway address. */
    std::string gatewayIP() const;
    /** @brief Returns the STA subnet mask. */
    std::string subnetMask() const;
    /** @brief Returns the STA MAC address. */
    std::string macAddressSTA() const;
    /** @brief Returns the current STA RSSI. */
    int8_t RSSI();
    /** @brief Returns the connected SSID. */
    std::string SSID();
    /** @brief Returns the connected BSSID. */
    std::string BSSID();
    /** @brief Returns the current channel. */
    uint8_t channel();
    /** @brief Returns the current STA auth mode. */
    wifi_auth_mode_t authMode();

    /** @brief Starts an access point. */
    esp_err_t softAP(const std::string& ssid, const std::string& password = "", uint8_t channel = 1,
                     bool hidden = false, uint8_t max_connection = 4);
    /** @brief Stops the access point. */
    esp_err_t softAPdisconnect(bool wifioff = false);
    /** @brief Returns the AP IP address. */
    std::string softAPIP() const;
    /** @brief Returns the AP MAC address. */
    std::string macAddressAP() const;
    /** @brief Returns the number of connected AP stations. */
    uint8_t softAPgetStationNum() const;

    /** @brief Runs a synchronous Wi-Fi scan. */
    esp_err_t scanNetworks(std::vector<WiFiScanResult>& results, bool show_hidden = false, uint32_t timeout_ms = 10000);

    /** @brief Starts an asynchronous Wi-Fi scan. */
    esp_err_t scanNetworksAsync(bool show_hidden = false);
    /** @brief Collects asynchronous scan results. */
    esp_err_t getScanResults(std::vector<WiFiScanResult>& results);

    /** @brief Registers a user event callback. */
    void onEvent(EventCallback cb);

private:
    static void eventHandlerStatic(void* arg, esp_event_base_t base, int32_t id, void* data);
    void eventHandler(esp_event_base_t base, int32_t id, void* data);

    void fireEvent(WiFiEvent ev, void* data);

    esp_err_t ensureStaForScan(WiFiMode& restore_mode);

private:
    bool _initialized       = false;
    WiFiMode _mode          = WiFiMode::OFF;
    esp_netif_t* _sta_netif = nullptr;
    esp_netif_t* _ap_netif  = nullptr;

    esp_event_handler_instance_t _wifi_handler_inst = nullptr;
    esp_event_handler_instance_t _ip_handler_inst   = nullptr;

    EventGroupHandle_t _event_group = nullptr;

    EventCallback _user_cb;

    volatile bool _sta_connected = false;
    volatile bool _sta_got_ip    = false;
    esp_ip4_addr_t _sta_ip{};
    esp_ip4_addr_t _sta_gw{};
    esp_ip4_addr_t _sta_netmask{};

    std::string _configured_ssid;
    std::string _configured_password;
};

extern WiFiManager WiFi;

}  // namespace hal_wifi
