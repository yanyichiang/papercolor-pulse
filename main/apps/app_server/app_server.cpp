/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "apps/app_server/app_server.h"
#include "apps/app_server/setup_routing.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <cstdint>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>
#include <strings.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/inet.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "esp_vfs_fat.h"
#include "tinyusb_msc.h"

#include "hal/wifi/hal_wifi.h"
#include "hal/storage/hal_storage.h"
#include "apps/local_photo_slideshow/local_photo_slideshow.h"
#include "hal/utils/dns_server/dns_server.h"
#include "hal/hal.h"
#include "mdns.h"
#include "apps/app_manager/app_manager.h"

using namespace hal_wifi;

extern PhotoSlideshow photo_slideshow;

#define TAG "app_server"

/* ==== Constants ==== */
#define BASE_PATH "/data"

#ifndef APP_ASSETS_USE_EMBEDDED
#define APP_ASSETS_USE_EMBEDDED 0
#endif

#if APP_ASSETS_USE_EMBEDDED
extern const char _binary_index_html_start[] asm("_binary_index_html_start");
extern const char _binary_index_html_end[] asm("_binary_index_html_end");
#endif

#define SCAN_TIMEOUT_MS    (8000)
#define CONNECT_TIMEOUT_MS (15000)
#define UPLOAD_MAX_SIZE    (2 * 1024 * 1024)
#define PUSH_MAX_SIZE      (768 * 1024)
#define PHOTOS_PER_PAGE    (16)
#define MAX_PHOTOS         (500)

#ifndef PAPERCOLOR_API_TOKEN
#define PAPERCOLOR_API_TOKEN ""
#endif

/* ==== Static state ==== */
static httpd_handle_t g_srv         = NULL;
static wifi_network_t *g_scan_cache = NULL;
static int g_scan_n                 = 0;

typedef struct {
    char name[64];
    char url[320];
    size_t size;
} photo_entry_t;

static device_state_t g_dev_state = {0};

class AppOperationGuard {
public:
    AppOperationGuard()
    {
        app_manager_operation_begin();
    }
    ~AppOperationGuard()
    {
        app_manager_operation_end();
    }

    AppOperationGuard(const AppOperationGuard &)            = delete;
    AppOperationGuard &operator=(const AppOperationGuard &) = delete;
};

static const struct {
    const char *id;
    const char *name;
} k_supported_modes[] = {
    {MODE_ID_LOCAL, "Photo Album"},
};

static const char *get_mime(const char *path)
{
    const char *d = strrchr(path, '.');
    if (!d) {
        return "application/octet-stream";
    }
    if (strcasecmp(d, ".jpg") == 0 || strcasecmp(d, ".jpeg") == 0) {
        return "image/jpeg";
    }
    if (strcasecmp(d, ".png") == 0) {
        return "image/png";
    }
    if (strcasecmp(d, ".bmp") == 0) {
        return "image/bmp";
    }
    if (strcasecmp(d, ".webp") == 0) {
        return "image/webp";
    }
    if (strcasecmp(d, ".gif") == 0) {
        return "image/gif";
    }
    if (strcasecmp(d, ".html") == 0) {
        return "text/html";
    }
    if (strcasecmp(d, ".css") == 0) {
        return "text/css";
    }
    if (strcasecmp(d, ".js") == 0) {
        return "application/javascript";
    }
    return "application/octet-stream";
}

static bool is_image_file(const char *name)
{
    const char *d = strrchr(name, '.');
    if (!d) {
        return false;
    }
    const char *ext[] = {"png", "jpg", "jpeg", "bmp", "webp", "gif", NULL};
    for (int i = 0; ext[i]; i++) {
        if (strcasecmp(d + 1, ext[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool detect_image_extension(const void *data, size_t len, char *ext, size_t ext_sz)
{
    if (!data || !ext || ext_sz < 4) {
        return false;
    }

    const uint8_t *bytes = static_cast<const uint8_t *>(data);
    if (len >= 8 && memcmp(bytes, "\x89PNG\r\n\x1A\n", 8) == 0) {
        strlcpy(ext, "png", ext_sz);
        return true;
    }
    if (len >= 3 && bytes[0] == 0xFF && bytes[1] == 0xD8 && bytes[2] == 0xFF) {
        strlcpy(ext, "jpg", ext_sz);
        return true;
    }
    if (len >= 2 && bytes[0] == 'B' && bytes[1] == 'M') {
        strlcpy(ext, "bmp", ext_sz);
        return true;
    }
    return false;
}

static bool url_encode_component(const char *src, char *dst, size_t dst_sz)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t j                = 0;

    if (!src || !dst || dst_sz == 0) {
        return false;
    }

    for (size_t i = 0; src[i]; i++) {
        unsigned char c = (unsigned char)src[i];

        bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
                    c == '_' || c == '.' || c == '~';

        if (safe) {
            if (j + 1 >= dst_sz) {
                return false;
            }
            dst[j++] = (char)c;
        } else {
            if (j + 3 >= dst_sz) {
                return false;
            }
            dst[j++] = '%';
            dst[j++] = hex[(c >> 4) & 0x0F];
            dst[j++] = hex[c & 0x0F];
        }
    }

    dst[j] = 0;
    return true;
}

static bool url_decode_component(const char *src, char *dst, size_t dst_sz)
{
    size_t j = 0;

    if (!src || !dst || dst_sz == 0) {
        return false;
    }

    for (size_t i = 0; src[i]; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '%') {
            if (!src[i + 1] || !src[i + 2]) {
                return false;
            }
            char hex[3] = {src[i + 1], src[i + 2], 0};
            char *end   = NULL;
            long value  = strtol(hex, &end, 16);
            if (end != hex + 2) {
                return false;
            }
            if (j + 1 >= dst_sz) {
                return false;
            }
            dst[j++] = (char)value;
            i += 2;
        } else if (c == '+') {
            if (j + 1 >= dst_sz) {
                return false;
            }
            dst[j++] = ' ';
        } else {
            if (j + 1 >= dst_sz) {
                return false;
            }
            dst[j++] = (char)c;
        }
    }

    dst[j] = 0;
    return true;
}

static void send_json_response(httpd_req_t *req, cJSON *root)
{
    char *s = cJSON_PrintUnformatted(root);
    if (!s) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"oom\"}");
        return;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, s);
    app_manager_mark_activity();
    free(s);
}

static esp_err_t send_error_response(httpd_req_t *req, int code, const char *msg)
{
    httpd_resp_set_status(req, code == 400   ? "400 Bad Request"
                               : code == 401 ? "401 Unauthorized"
                               : code == 403 ? "403 Forbidden"
                               : code == 404 ? "404 Not Found"
                               : code == 413 ? "413 Payload Too Large"
                               : code == 503 ? "503 Service Unavailable"
                                             : "500 Internal Server Error");
    httpd_resp_set_type(req, "application/json");
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "status", "error");
    cJSON_AddStringToObject(r, "message", msg);
    char *s = cJSON_PrintUnformatted(r);
    if (s) {
        httpd_resp_sendstr(req, s);
        free(s);
    } else {
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"oom\"}");
    }
    cJSON_Delete(r);
    return ESP_FAIL;
}

static esp_err_t require_machine_auth(httpd_req_t *req)
{
    if (PAPERCOLOR_API_TOKEN[0] == 0) {
        return send_error_response(req, 503, "machine api token not configured");
    }

    char auth[192] = {0};
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth, sizeof(auth)) != ESP_OK) {
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Bearer");
        return send_error_response(req, 401, "missing bearer token");
    }

    static constexpr const char *prefix = "Bearer ";
    size_t prefix_len                   = strlen(prefix);
    if (strncmp(auth, prefix, prefix_len) != 0 || strcmp(auth + prefix_len, PAPERCOLOR_API_TOKEN) != 0) {
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Bearer");
        return send_error_response(req, 401, "invalid bearer token");
    }
    return ESP_OK;
}

static esp_err_t send_file_response(httpd_req_t *req, const char *path)
{
    hal_storage_lock();
    FILE *f = fopen(path, "rb");
    if (!f) {
        hal_storage_unlock();
        return send_error_response(req, 404, "not found");
    }
    httpd_resp_set_type(req, get_mime(path));

    char buf[2048];
    esp_err_t err = ESP_OK;
    while (true) {
        size_t n = fread(buf, 1, sizeof(buf), f);
        if (n > 0) {
            err = httpd_resp_send_chunk(req, buf, n);
            if (err != ESP_OK) break;
        }
        if (n < sizeof(buf)) {
            if (ferror(f)) {
                err = ESP_FAIL;
            }
            break;
        }
    }
    fclose(f);
    hal_storage_unlock();
    if (err != ESP_OK) {
        return err;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

/* ==== Image naming (image001.jpg … image999.jpg) ==== */
static esp_err_t generate_next_photo_name(const char *ext, const char *algorithm, char *out, size_t sz)
{
    char prefix     = (algorithm && strcmp(algorithm, "nearest") == 0) ? 'N' : 'd';
    bool used[1000] = {0};
    DIR *d          = opendir(BASE_PATH);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            unsigned n;
            char pf;
            if (e->d_type == DT_REG && sscanf(e->d_name, "image%c%3u", &pf, &n) == 2 && pf == prefix) {
                used[n] = true;
            }
        }
        closedir(d);
    }
    for (unsigned n = 1; n <= 999; n++) {
        if (!used[n]) {
            snprintf(out, sz, "image%c%03u.%s", prefix, n, ext);
            return ESP_OK;
        }
    }
    return ESP_FAIL;
}

/* ==== WiFi (via hal_wifi library) ==== */
static int perform_wifi_scan(void)
{
    // Pause auto-reconnect to avoid scan contention causing esp_wifi_scan_start to fail
    app_manager_pause_wifi_reconnect();

    std::vector<hal_wifi::WiFiScanResult> results;
    esp_err_t err = WiFi.scanNetworks(results, false, SCAN_TIMEOUT_MS);

    app_manager_resume_wifi_reconnect();

    if (err != ESP_OK) {
        return -1;
    }

    wifi_network_t *cache = (wifi_network_t *)malloc(results.size() * sizeof(wifi_network_t));
    int cnt               = 0;
    if (cache) {
        for (size_t i = 0; i < results.size(); i++) {
            strlcpy(cache[i].ssid, results[i].ssid.c_str(), sizeof(cache[i].ssid));
            cache[i].rssi   = results[i].rssi;
            cache[i].secure = results[i].authmode != WIFI_AUTH_OPEN;
            cnt++;
        }
    }
    if (g_scan_cache) {
        free(g_scan_cache);
    }
    g_scan_cache = cache;
    g_scan_n     = cnt;
    return cnt;
}

static void app_server_set_requested_mode(const char *mode_id)
{
    if (mode_id && mode_id[0]) {
        strlcpy(g_dev_state.requested_mode, mode_id, sizeof(g_dev_state.requested_mode));
    } else {
        g_dev_state.requested_mode[0] = 0;
    }
}

static void app_server_set_connecting_state(const char *mode_id)
{
    if (mode_id && mode_id[0]) {
        strlcpy(g_dev_state.requested_mode, mode_id, sizeof(g_dev_state.requested_mode));
    }
    strlcpy(g_dev_state.conn_err, "connecting", sizeof(g_dev_state.conn_err));
}

static void app_server_clear_connecting_state(void)
{
    g_dev_state.conn_err[0] = 0;
}

static void app_server_set_mode_state(const char *mode_id)
{
    strlcpy(g_dev_state.current_mode, mode_id, sizeof(g_dev_state.current_mode));
    if (mode_id && mode_id[0]) {
        strlcpy(g_dev_state.requested_mode, mode_id, sizeof(g_dev_state.requested_mode));
    }
    g_dev_state.conn_err[0] = 0;
}

static void app_server_set_mode_pending_state(const char *mode_id)
{
    app_server_set_requested_mode(mode_id);
}

static void app_server_set_connect_failed_state(const char *message)
{
    strlcpy(g_dev_state.conn_err, message, sizeof(g_dev_state.conn_err));
}

static int perform_wifi_connect(const char *ssid, const char *pass)
{
    // Pause auto-reconnect to avoid connection contention
    app_manager_pause_wifi_reconnect();
    WiFi.disconnect();
    vTaskDelay(pdMS_TO_TICKS(200));

    esp_err_t err = WiFi.connect(ssid, pass ? pass : "", CONNECT_TIMEOUT_MS);

    app_manager_resume_wifi_reconnect();

    if (err == ESP_OK) {
        app_server_clear_connecting_state();
        return 0;
    }

    if (err == ESP_ERR_TIMEOUT) {
        app_server_set_connect_failed_state("connection timeout");
    } else {
        app_server_set_connect_failed_state("connection failed");
    }
    return -1;
}

/* ==== Route handlers ==== */
static esp_err_t h_get_modes(httpd_req_t *req)
{
    cJSON *r = cJSON_CreateObject();
    cJSON *a = cJSON_AddArrayToObject(r, "modes");

    for (size_t i = 0; i < sizeof(k_supported_modes) / sizeof(k_supported_modes[0]); ++i) {
        cJSON *m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "id", k_supported_modes[i].id);
        cJSON_AddStringToObject(m, "name", k_supported_modes[i].name);
        cJSON_AddItemToArray(a, m);
    }

    send_json_response(req, r);
    cJSON_Delete(r);
    return ESP_OK;
}

static esp_err_t h_wifi_scan(httpd_req_t *req)
{
    perform_wifi_scan();
    cJSON *r = cJSON_CreateObject();
    cJSON *a = cJSON_AddArrayToObject(r, "networks");

    for (int i = 0; i < g_scan_n; i++) {
        cJSON *n = cJSON_CreateObject();
        cJSON_AddStringToObject(n, "ssid", g_scan_cache[i].ssid);
        cJSON_AddNumberToObject(n, "rssi", g_scan_cache[i].rssi);
        cJSON_AddBoolToObject(n, "secure", g_scan_cache[i].secure);
        cJSON_AddItemToArray(a, n);
    }

    send_json_response(req, r);
    cJSON_Delete(r);
    return ESP_OK;
}

static esp_err_t h_wifi_config(httpd_req_t *req)
{
    char buf[512];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        return send_error_response(req, 400, "no body");
    }
    buf[len] = 0;

    cJSON *j = cJSON_Parse(buf);
    if (!j) {
        return send_error_response(req, 400, "bad json");
    }

    cJSON *ssid_j        = cJSON_GetObjectItem(j, "ssid");
    cJSON *pass_j        = cJSON_GetObjectItem(j, "password");
    cJSON *mode_j        = cJSON_GetObjectItem(j, "mode");
    cJSON *boot_sound_j  = cJSON_GetObjectItem(j, "boot_sound");
    cJSON *device_name_j = cJSON_GetObjectItem(j, "device_name");

    const bool has_ssid        = cJSON_IsString(ssid_j) && ssid_j->valuestring;
    const bool has_boot_sound  = cJSON_IsBool(boot_sound_j);
    const bool has_device_name = cJSON_IsString(device_name_j) && device_name_j->valuestring;
    const bool boot_sound      = has_boot_sound && cJSON_IsTrue(boot_sound_j);

    if (!has_ssid && !has_boot_sound && !has_device_name &&
        !(cJSON_IsString(mode_j) && mode_j->valuestring && mode_j->valuestring[0])) {
        cJSON_Delete(j);
        return send_error_response(req, 400, "ssid required");
    }

    char ssid_buf[64]        = {0};
    char pass_buf[64]        = {0};
    char mode_buf[32]        = {0};
    char device_name_buf[64] = {0};

    if (has_ssid) {
        strlcpy(ssid_buf, ssid_j->valuestring, sizeof(ssid_buf));
    }
    if (cJSON_IsString(pass_j) && pass_j->valuestring) {
        strlcpy(pass_buf, pass_j->valuestring, sizeof(pass_buf));
    }
    if (cJSON_IsString(mode_j) && mode_j->valuestring && mode_j->valuestring[0]) {
        strlcpy(mode_buf, mode_j->valuestring, sizeof(mode_buf));
    }
    if (has_device_name) {
        normalize_device_name(device_name_j->valuestring, device_name_buf, sizeof(device_name_buf));
    }
    cJSON_Delete(j);

    if (mode_buf[0]) {
        app_server_set_mode_pending_state(mode_buf);
    }

    if (has_ssid) {
        char connect_pass[sizeof(hal.settings.wifi_password)] = {0};
        if (pass_buf[0]) {
            strlcpy(connect_pass, pass_buf, sizeof(connect_pass));
        } else {
            hal.settingsLock();
            strlcpy(connect_pass, hal.settings.wifi_password, sizeof(connect_pass));
            hal.settingsUnlock();
        }

        // Skip reconnect if the SSID and password are unchanged and already connected
        bool skip_connect = false;
        if (WiFi.isConnected()) {
            hal.settingsLock();
            skip_connect = (strcmp(ssid_buf, hal.settings.wifi_ssid) == 0 &&
                            strcmp(connect_pass, hal.settings.wifi_password) == 0);
            hal.settingsUnlock();
        }

        strlcpy(hal.settings.wifi_ssid, ssid_buf, sizeof(hal.settings.wifi_ssid));
        if (pass_buf[0]) {
            strlcpy(hal.settings.wifi_password, pass_buf, sizeof(hal.settings.wifi_password));
        }
        hal.settingsSave(SETTING_WIFI_SSID);
        if (pass_buf[0]) {
            hal.settingsSave(SETTING_WIFI_PASSWORD);
        }

        if (!skip_connect) {
            app_server_set_connecting_state(mode_buf[0] ? mode_buf : nullptr);
            if (perform_wifi_connect(ssid_buf, connect_pass) != 0) {
                return send_error_response(req, 500, "wifi connect failed");
            }
        } else {
            app_server_clear_connecting_state();
        }
    }

    if (mode_buf[0]) {
        esp_err_t mode_err = app_manager_apply_mode(mode_buf);
        if (mode_err != ESP_OK) {
            return send_error_response(req, 500, "mode switch failed");
        }
        app_server_set_mode_state(mode_buf);
    }

    if (has_boot_sound || has_device_name) {
        hal.settingsLock();
        if (has_boot_sound) {
            hal.settings.boot_sound = boot_sound;
        }
        if (has_device_name) {
            cstring_copy(hal.settings.device_name, device_name_buf, sizeof(hal.settings.device_name));
        }
        hal.settingsUnlock();

        if (has_boot_sound) {
            hal.settingsSave(SETTING_BOOT_SOUND);
        }
        if (has_device_name) {
            hal.settingsSave(SETTING_DEVICE_NAME);
            if (mdns_hostname_set(device_name_buf) != ESP_OK) {
                ESP_LOGW(TAG, "Failed to update mDNS hostname immediately");
            }
        }
    }

    bool saved_boot_sound                                    = false;
    char saved_device_name[sizeof(hal.settings.device_name)] = {0};
    hal.settingsLock();
    saved_boot_sound = hal.settings.boot_sound;
    cstring_copy(saved_device_name, hal.settings.device_name, sizeof(saved_device_name));
    hal.settingsUnlock();

    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "status", "ok");
    cJSON_AddBoolToObject(r, "boot_sound", saved_boot_sound);
    cJSON_AddStringToObject(r, "device_name", saved_device_name);
    send_json_response(req, r);
    cJSON_Delete(r);
    return ESP_OK;
}

static esp_err_t h_wifi_status(httpd_req_t *req)
{
    cJSON *r                                                = cJSON_CreateObject();
    std::string ssid                                        = WiFi.SSID();
    bool boot_sound                                         = false;
    char device_name[sizeof(hal.settings.device_name)]      = {0};
    char current_mode[sizeof(g_dev_state.current_mode)]     = {0};
    char requested_mode[sizeof(g_dev_state.requested_mode)] = {0};
    char conn_err[sizeof(g_dev_state.conn_err)]             = {0};
    bool connected                                          = WiFi.isConnected();

    hal.settingsLock();
    boot_sound = hal.settings.boot_sound;
    cstring_copy(device_name, hal.settings.device_name, sizeof(device_name));
    hal.settingsUnlock();

    strlcpy(current_mode, g_dev_state.current_mode, sizeof(current_mode));
    strlcpy(requested_mode, g_dev_state.requested_mode, sizeof(requested_mode));
    strlcpy(conn_err, g_dev_state.conn_err, sizeof(conn_err));

    cJSON_AddBoolToObject(r, "connected", connected);
    cJSON_AddStringToObject(r, "mode", current_mode);
    cJSON_AddStringToObject(r, "requested_mode", requested_mode);
    cJSON_AddStringToObject(r, "ssid", ssid.c_str());
    cJSON_AddStringToObject(r, "error", conn_err[0] ? conn_err : NULL);

    if (connected) {
        std::string ip = WiFi.localIP();
        cJSON_AddStringToObject(r, "ip", ip.c_str());
    }
    cJSON_AddBoolToObject(r, "boot_sound", boot_sound);
    cJSON_AddStringToObject(r, "device_name", device_name);
    send_json_response(req, r);
    cJSON_Delete(r);
    return ESP_OK;
}

static esp_err_t h_device_ready(httpd_req_t *req)
{
    cJSON *r    = cJSON_CreateObject();
    cJSON *wifi = cJSON_AddObjectToObject(r, "wifi");
    cJSON *mode = cJSON_AddObjectToObject(r, "mode");
    cJSON *ui   = cJSON_AddObjectToObject(r, "ui");

    bool connected                                          = WiFi.isConnected();
    std::string ssid                                        = WiFi.SSID();
    std::string ip                                          = connected ? WiFi.localIP() : std::string();
    char current_mode[sizeof(g_dev_state.current_mode)]     = {0};
    char requested_mode[sizeof(g_dev_state.requested_mode)] = {0};
    char conn_err[sizeof(g_dev_state.conn_err)]             = {0};

    strlcpy(current_mode, g_dev_state.current_mode, sizeof(current_mode));
    strlcpy(requested_mode, g_dev_state.requested_mode, sizeof(requested_mode));
    strlcpy(conn_err, g_dev_state.conn_err, sizeof(conn_err));

    // Only LOCAL mode exists in this build; it does not depend on WiFi being
    // up, so as soon as the requested mode is the live one we're ready.
    const bool mode_ready     = requested_mode[0] && strcmp(current_mode, requested_mode) == 0;
    const bool can_enter_mode = mode_ready;

    const char *wifi_state = "disconnected";
    if (conn_err[0]) {
        wifi_state = strcmp(conn_err, "connecting") == 0 ? "connecting" : "failed";
    } else if (connected) {
        wifi_state = "connected";
    }

    cJSON_AddStringToObject(wifi, "state", wifi_state);
    cJSON_AddStringToObject(wifi, "ssid", ssid.c_str());
    cJSON_AddStringToObject(wifi, "ip", connected ? ip.c_str() : "");
    cJSON_AddStringToObject(wifi, "error", (conn_err[0] && strcmp(conn_err, "connecting") != 0) ? conn_err : NULL);
    cJSON_AddStringToObject(mode, "requested", requested_mode);
    cJSON_AddStringToObject(mode, "current", current_mode);
    cJSON_AddBoolToObject(mode, "ready", mode_ready);
    cJSON_AddBoolToObject(ui, "can_enter_mode", can_enter_mode);
    cJSON_AddBoolToObject(ui, "requires_wifi_setup", papercolor::requires_wifi_setup(connected));

    send_json_response(req, r);
    cJSON_Delete(r);
    return ESP_OK;
}

static esp_err_t h_hardware_diagnostics_get(httpd_req_t *req)
{
    esp_err_t auth_err = require_machine_auth(req);
    if (auth_err != ESP_OK) return auth_err;

    float temperature = 0.0f;
    float humidity    = 0.0f;
    bool sht40_ok     = hal.sht40CachedRead(&temperature, &humidity);

    cJSON *r       = cJSON_CreateObject();
    cJSON *buttons = cJSON_AddObjectToObject(r, "buttons");
    cJSON *sht40   = cJSON_AddObjectToObject(r, "sht40");
    cJSON *power   = cJSON_AddObjectToObject(r, "power");
    cJSON *audio   = cJSON_AddObjectToObject(r, "audio");
    cJSON *storage = cJSON_AddObjectToObject(r, "storage");
    cJSON *leds    = cJSON_AddObjectToObject(r, "leds");

    cJSON_AddBoolToObject(buttons, "a", M5.BtnA.isPressed());
    cJSON_AddBoolToObject(buttons, "b", M5.BtnB.isPressed());
    cJSON_AddBoolToObject(buttons, "c", M5.BtnC.isPressed());
    cJSON_AddBoolToObject(sht40, "ok", sht40_ok);
    if (sht40_ok) {
        cJSON_AddNumberToObject(sht40, "temperature_c", temperature);
        cJSON_AddNumberToObject(sht40, "humidity_percent", humidity);
    } else {
        cJSON_AddNullToObject(sht40, "temperature_c");
        cJSON_AddNullToObject(sht40, "humidity_percent");
    }
    cJSON_AddBoolToObject(power, "battery_ok", hal.battery_ok);
    cJSON_AddNumberToObject(power, "battery_mv", hal.battery_mv);
    cJSON_AddBoolToObject(power, "vin_ok", hal.vin_ok);
    cJSON_AddNumberToObject(power, "vin_mv", hal.vin_mv);
    cJSON_AddBoolToObject(power, "external", hal.vin_ok && hal.vin_mv >= 4000);
    cJSON_AddBoolToObject(audio, "microphone_available", M5.Mic.isEnabled());
    cJSON_AddBoolToObject(audio, "speaker_available", M5.Speaker.isEnabled());
    cJSON_AddBoolToObject(storage, "sd_inserted", hal.sd_inserted);
    cJSON_AddNumberToObject(storage, "rtc_epoch_utc", static_cast<double>(time(nullptr)));
    cJSON_AddNumberToObject(leds, "count", M5.Led.getCount());

    send_json_response(req, r);
    cJSON_Delete(r);
    return ESP_OK;
}

static esp_err_t h_hardware_diagnostics_post(httpd_req_t *req)
{
    esp_err_t auth_err = require_machine_auth(req);
    if (auth_err != ESP_OK) return auth_err;
    if (!hal.externalPowerPresent()) {
        return send_error_response(req, 409, "hardware tests require external power");
    }

    char buf[128];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) return send_error_response(req, 400, "no body");
    buf[len] = 0;

    cJSON *j = cJSON_Parse(buf);
    if (!j) return send_error_response(req, 400, "bad json");
    cJSON *action_j = cJSON_GetObjectItem(j, "action");
    if (!cJSON_IsString(action_j) || !action_j->valuestring) {
        cJSON_Delete(j);
        return send_error_response(req, 400, "action required");
    }

    char action[24] = {0};
    strlcpy(action, action_j->valuestring, sizeof(action));
    cJSON_Delete(j);

    AppOperationGuard operation_guard;
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "action", action);

    if (strcmp(action, "microphone") == 0) {
        MicrophoneDiagnostics result;
        bool ok = hal.microphoneDiagnostics(&result);
        cJSON_AddBoolToObject(r, "ok", ok);
        cJSON_AddNumberToObject(r, "sample_count", result.sample_count);
        cJSON_AddNumberToObject(r, "dc_offset", result.dc_offset);
        cJSON_AddNumberToObject(r, "peak", result.peak);
        cJSON_AddNumberToObject(r, "rms", result.rms);
    } else if (strcmp(action, "speaker") == 0) {
        cJSON_AddBoolToObject(r, "ok", hal.speakerSelfTest());
    } else if (strcmp(action, "leds") == 0) {
        size_t count = hal.rgbLedSelfTest();
        cJSON_AddBoolToObject(r, "ok", count > 0);
        cJSON_AddNumberToObject(r, "count", count);
    } else {
        cJSON_Delete(r);
        return send_error_response(req, 400, "unknown action");
    }

    send_json_response(req, r);
    cJSON_Delete(r);
    return ESP_OK;
}

static esp_err_t h_wifi_disconnect(httpd_req_t *req)
{
    app_manager_disconnect_sta_keep_ap();
    g_dev_state.current_mode[0]   = 0;
    g_dev_state.requested_mode[0] = 0;
    g_dev_state.conn_err[0]       = 0;
    cJSON *r                      = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "status", "disconnected");
    send_json_response(req, r);
    cJSON_Delete(r);
    return ESP_OK;
}

static esp_err_t h_photos_list(httpd_req_t *req)
{
    int page = 1, per = PHOTOS_PER_PAGE;
    char query_buffer[64];
    if (httpd_req_get_url_query_str(req, query_buffer, sizeof(query_buffer)) == ESP_OK) {
        char page_buffer[16];
        if (httpd_query_key_value(query_buffer, "page", page_buffer, sizeof(page_buffer)) == ESP_OK) {
            int page_value = atoi(page_buffer);
            if (page_value >= 1) {
                page = page_value;
            }
        }
        if (httpd_query_key_value(query_buffer, "per_page", page_buffer, sizeof(page_buffer)) == ESP_OK) {
            int per_page_value = atoi(page_buffer);
            if (per_page_value >= 1) {
                per = per_page_value;
            }
        }
    }

    photo_entry_t *photos = (photo_entry_t *)calloc(MAX_PHOTOS, sizeof(photo_entry_t));
    if (!photos) {
        return send_error_response(req, 500, "oom");
    }

    int total = 0;
    hal_storage_prepare_photo_fs_access();
    hal_storage_lock();
    DIR *directory = opendir(BASE_PATH);
    if (directory) {
        struct dirent *entry;
        while ((entry = readdir(directory)) != NULL && total < MAX_PHOTOS) {
            if (entry->d_type != DT_REG || !is_image_file(entry->d_name)) {
                continue;
            }
            char file_path[PATH_MAX];
            snprintf(file_path, sizeof(file_path), "%s/%s", BASE_PATH, entry->d_name);
            struct stat st;
            if (stat(file_path, &st) != 0) {
                continue;
            }
            strlcpy(photos[total].name, entry->d_name, sizeof(photos[total].name));
            char encoded_name[sizeof(photos[total].url) - sizeof("/data/")] = {0};
            if (!url_encode_component(entry->d_name, encoded_name, sizeof(encoded_name))) {
                continue;
            }
            if (snprintf(photos[total].url, sizeof(photos[total].url), "/data/%s", encoded_name) >=
                (int)sizeof(photos[total].url)) {
                continue;
            }
            photos[total].size = st.st_size;
            total++;
        }
        closedir(directory);
    }
    hal_storage_unlock();

    for (int i = 1; i < total; i++) {
        photo_entry_t current_photo = photos[i];
        int insert_index            = i - 1;
        while (insert_index >= 0 && strcmp(photos[insert_index].name, current_photo.name) > 0) {
            photos[insert_index + 1] = photos[insert_index];
            insert_index--;
        }
        photos[insert_index + 1] = current_photo;
    }

    int pages = total > 0 ? (total + per - 1) / per : 1;
    if (page > pages) {
        page = pages;
    }
    int start = (page - 1) * per;
    int end   = start + per;
    if (end > total) {
        end = total;
    }

    cJSON *r = cJSON_CreateObject();
    cJSON *a = cJSON_AddArrayToObject(r, "photos");
    for (int i = start; i < end; i++) {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "name", photos[i].name);
        cJSON_AddStringToObject(p, "url", photos[i].url);
        cJSON_AddNumberToObject(p, "size", (double)photos[i].size);
        cJSON_AddItemToArray(a, p);
    }
    cJSON_AddNumberToObject(r, "total", total);
    cJSON_AddNumberToObject(r, "page", page);
    cJSON_AddNumberToObject(r, "per_page", per);
    cJSON_AddNumberToObject(r, "total_pages", pages);
    send_json_response(req, r);
    cJSON_Delete(r);
    free(photos);
    return ESP_OK;
}

static esp_err_t h_photos_upload(httpd_req_t *req)
{
    AppOperationGuard operation_guard;
    if (req->content_len > UPLOAD_MAX_SIZE) {
        return send_error_response(req, 400, "file too large");
    }

    char *body = (char *)malloc(req->content_len + 1);
    if (!body) {
        return send_error_response(req, 500, "oom");
    }
    size_t left = req->content_len, off = 0;
    while (left > 0) {
        int ret = httpd_req_recv(req, body + off, left);
        if (ret <= 0) {
            free(body);
            return send_error_response(req, 500, "read err");
        }
        off += ret;
        left -= ret;
    }
    body[off] = 0;

    char ct[256];
    const char *p = NULL;
    if (httpd_req_get_hdr_value_str(req, "Content-Type", ct, sizeof(ct)) == ESP_OK) {
        p = strstr(ct, "boundary=");
    }
    if (!p) {
        free(body);
        return send_error_response(req, 400, "no boundary");
    }
    p += 9;
    if (*p == '"') {
        p++;
    }
    char bd[128];
    int bi = 0;
    while (*p && *p != ' ' && *p != ';' && *p != '"' && bi < (int)sizeof(bd) - 1) {
        bd[bi++] = *p++;
    }
    bd[bi] = 0;

    char bd_marker[132];
    snprintf(bd_marker, sizeof(bd_marker), "--%s", bd);
    char bd_sep[136];
    snprintf(bd_sep, sizeof(bd_sep), "\r\n--%s", bd);
    size_t bdl  = strlen(bd_marker);
    size_t bdsl = strlen(bd_sep);

    auto mem_find = [&](const char *hay, size_t hlen, const char *needle, size_t nlen) -> const char * {
        if (nlen > hlen) {
            return NULL;
        }
        for (size_t i = 0; i <= hlen - nlen; i++) {
            if (memcmp(hay + i, needle, nlen) == 0) {
                return hay + i;
            }
        }
        return NULL;
    };

    const char *filedata = NULL;
    size_t flen          = 0;
    char action[32]      = "upload_only";
    char algorithm[16]   = "dither";
    const char *pos = body, *end = body + off;
    while (pos < end) {
        size_t remaining = end - pos;
        const char *s    = mem_find(pos, remaining, bd_marker, bdl);
        if (!s) {
            break;
        }
        const char *part = s + bdl;
        remaining        = end - part;
        if (remaining >= 2 && *part == '\r' && *(part + 1) == '\n') {
            part += 2;
            remaining -= 2;
        } else if (remaining >= 2 && *part == '-' && *(part + 1) == '-') {
            break;
        }

        const char *hdr_end = mem_find(part, remaining, "\r\n\r\n", 4);
        if (!hdr_end) {
            break;
        }
        hdr_end += 4;
        remaining = end - hdr_end;

        const char *next = NULL;
        {
            const char *sp = hdr_end;
            size_t sr      = remaining;
            while (sp < end) {
                sp = mem_find(sp, sr, bd_sep, bdsl);
                if (!sp) {
                    break;
                }
                next = sp + 2;
                break;
            }
        }

        const char *val_end = next ? next : end;
        while (val_end > hdr_end && (val_end[-1] == '\n' || val_end[-1] == '\r')) {
            val_end--;
        }
        size_t vlen = val_end - hdr_end;

        if (mem_find(part, hdr_end - part, "name=\"file\"", 11) && mem_find(part, hdr_end - part, "filename=\"", 10)) {
            filedata = hdr_end;
            flen     = vlen;
        } else if (mem_find(part, hdr_end - part, "name=\"action\"", 13)) {
            if (vlen >= sizeof(action)) {
                vlen = sizeof(action) - 1;
            }
            memcpy(action, hdr_end, vlen);
            action[vlen] = 0;
        } else if (mem_find(part, hdr_end - part, "name=\"algorithm\"", 16)) {
            if (vlen >= sizeof(algorithm)) {
                vlen = sizeof(algorithm) - 1;
            }
            memcpy(algorithm, hdr_end, vlen);
            algorithm[vlen] = 0;
        }
        pos = next ? next : end;
    }

    if (!filedata || flen == 0) {
        free(body);
        return send_error_response(req, 400, "no file");
    }

    char ext[16] = {0};
    if (!detect_image_extension(filedata, flen, ext, sizeof(ext))) {
        free(body);
        return send_error_response(req, 400, "unsupported image type");
    }

    char fname[64];
    hal_storage_prepare_photo_fs_access();
    hal_storage_lock();
    if (generate_next_photo_name(ext, algorithm, fname, sizeof(fname)) != ESP_OK) {
        hal_storage_unlock();
        free(body);
        return send_error_response(req, 500, "name gen fail");
    }

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", BASE_PATH, fname);
    FILE *f = fopen(path, "wb");
    if (!f) {
        hal_storage_unlock();
        free(body);
        return send_error_response(req, 500, "write fail");
    }
    fwrite(filedata, 1, flen, f);
    fclose(f);
    hal_storage_unlock();

    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "status", "ok");
    cJSON_AddStringToObject(r, "name", fname);

    if (strcmp(action, "upload_display") == 0) {
        photo_slideshow.displayPhotoByPath(path);
        strlcpy(g_dev_state.current_image, fname, sizeof(g_dev_state.current_image));
        cJSON_AddStringToObject(r, "displaying", fname);
    }

    send_json_response(req, r);
    cJSON_Delete(r);
    free(body);
    return ESP_OK;
}

static esp_err_t h_photos_delete(httpd_req_t *req)
{
    AppOperationGuard operation_guard;
    char query_buffer[128];
    if (httpd_req_get_url_query_str(req, query_buffer, sizeof(query_buffer)) != ESP_OK) {
        return send_error_response(req, 400, "name required");
    }
    char name[64];
    if (httpd_query_key_value(query_buffer, "name", name, sizeof(name)) != ESP_OK) {
        return send_error_response(req, 400, "name required");
    }

    char decoded_name[64];
    if (!url_decode_component(name, decoded_name, sizeof(decoded_name))) {
        return send_error_response(req, 400, "invalid name");
    }
    if (strstr(decoded_name, "..") || strchr(decoded_name, '/') || strchr(decoded_name, '\\')) {
        return send_error_response(req, 403, "invalid path");
    }

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", BASE_PATH, decoded_name);
    hal_storage_prepare_photo_fs_access();
    hal_storage_lock();
    int rc = unlink(path);
    hal_storage_unlock();
    if (rc != 0) {
        return send_error_response(req, 404, "not found");
    }

    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "status", "deleted");
    send_json_response(req, r);
    cJSON_Delete(r);
    return ESP_OK;
}

static esp_err_t h_storage(httpd_req_t *req)
{
    uint64_t total = 0;
    uint64_t free  = 0;
    uint64_t used  = 0;

    hal_storage_prepare_photo_fs_access();
    if (esp_vfs_fat_info(BASE_PATH, &total, &free) == ESP_OK) {
        used = total - free;
    } else {
        ESP_LOGW(TAG, "esp_vfs_fat_info failed");
    }

    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "total", (double)total);
    cJSON_AddNumberToObject(r, "used", (double)used);
    cJSON_AddNumberToObject(r, "free", (double)free);

    send_json_response(req, r);
    cJSON_Delete(r);

    return ESP_OK;
}

static esp_err_t h_battery(httpd_req_t *req)
{
    uint16_t mv = 0;

    // read battery voltage via PM1 driver
    m5pm1_err_t err = hal.pm1.readVbat(&mv);
    if (err != M5PM1_OK) {
        mv = 0;
    }

    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "voltage_mv", (double)mv);

    send_json_response(req, r);
    cJSON_Delete(r);

    return ESP_OK;
}

static esp_err_t h_mode_cfg_get(httpd_req_t *req)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "orientation", hal.settings.rotation == 0 ? "landscape" : "portrait");
    cJSON_AddBoolToObject(r, "auto_slideshow", hal.settings.auto_slideshow);
    cJSON_AddNumberToObject(r, "interval_minutes", hal.settings.interval_minutes);
    cJSON_AddBoolToObject(r, "low_power_mode", hal.settings.low_power_mode);
    send_json_response(req, r);
    cJSON_Delete(r);
    return ESP_OK;
}

static esp_err_t h_mode_cfg_set(httpd_req_t *req)
{
    char buf[256];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        return send_error_response(req, 400, "no body");
    }
    buf[len] = 0;
    cJSON *j = cJSON_Parse(buf);
    if (!j) {
        return send_error_response(req, 400, "bad json");
    }

    bool orientation_changed = false;
    bool slideshow_changed   = false;
    bool interval_changed    = false;
    bool low_power_changed   = false;

    hal.settingsLock();
    cJSON *value_item;
    value_item = cJSON_GetObjectItem(j, "orientation");
    if (cJSON_IsString(value_item)) {
        uint8_t new_rotation = (strcmp(value_item->valuestring, "portrait") == 0) ? 1 : 0;
        if (new_rotation != hal.settings.rotation) {
            hal.settings.rotation = new_rotation;
            orientation_changed   = true;
        }
    }
    value_item = cJSON_GetObjectItem(j, "auto_slideshow");
    if (cJSON_IsBool(value_item) && (cJSON_IsTrue(value_item) != hal.settings.auto_slideshow)) {
        hal.settings.auto_slideshow = cJSON_IsTrue(value_item);
        slideshow_changed           = true;
    }
    value_item = cJSON_GetObjectItem(j, "interval_minutes");
    if (cJSON_IsNumber(value_item) && value_item->valueint >= 1 &&
        value_item->valueint != hal.settings.interval_minutes) {
        hal.settings.interval_minutes = value_item->valueint;
        interval_changed              = true;
    }
    value_item = cJSON_GetObjectItem(j, "low_power_mode");
    if (cJSON_IsBool(value_item) && (cJSON_IsTrue(value_item) != hal.settings.low_power_mode)) {
        hal.settings.low_power_mode = cJSON_IsTrue(value_item);
        low_power_changed           = true;
    }

    char orientation_response[16];
    bool slideshow_response;
    int interval_response;
    bool low_power_response;
    strlcpy(orientation_response, hal.settings.rotation == 0 ? "landscape" : "portrait", sizeof(orientation_response));
    slideshow_response = hal.settings.auto_slideshow;
    interval_response  = hal.settings.interval_minutes;
    low_power_response = hal.settings.low_power_mode;
    hal.settingsUnlock();
    cJSON_Delete(j);

    if (orientation_changed) {
        hal.settingsSave(SETTING_ROTATION);
    }
    if (slideshow_changed) {
        hal.settingsSave(SETTING_AUTO_SLIDESHOW);
    }
    if (interval_changed) {
        hal.settingsSave(SETTING_INTERVAL);
    }
    if (low_power_changed) {
        hal.settingsSave(SETTING_LOW_POWER_MODE);
    }

    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "orientation", orientation_response);
    cJSON_AddBoolToObject(r, "auto_slideshow", slideshow_response);
    cJSON_AddNumberToObject(r, "interval_minutes", interval_response);
    cJSON_AddBoolToObject(r, "low_power_mode", low_power_response);
    send_json_response(req, r);
    cJSON_Delete(r);
    return ESP_OK;
}

static esp_err_t h_mode_switch(httpd_req_t *req)
{
    char buf[256];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        return send_error_response(req, 400, "no body");
    }
    buf[len] = 0;
    cJSON *j = cJSON_Parse(buf);
    if (!j) {
        return send_error_response(req, 400, "bad json");
    }
    cJSON *mode_item = cJSON_GetObjectItem(j, "mode");
    if (!cJSON_IsString(mode_item)) {
        cJSON_Delete(j);
        return send_error_response(req, 400, "mode required");
    }

    char normalized_mode[16] = {0};
    normalize_mode_id(mode_item->valuestring, normalized_mode, sizeof(normalized_mode));
    if (!is_supported_mode_id(normalized_mode) || strcmp(mode_item->valuestring, normalized_mode) != 0) {
        cJSON_Delete(j);
        return send_error_response(req, 400, "invalid mode");
    }

    esp_err_t err = app_manager_apply_mode(normalized_mode);
    if (err != ESP_OK) {
        cJSON_Delete(j);
        return send_error_response(req, 500, "mode switch failed");
    }

    app_server_set_mode_state(normalized_mode);

    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "status", "ok");
    cJSON_AddStringToObject(r, "mode", normalized_mode);
    send_json_response(req, r);
    cJSON_Delete(r);
    cJSON_Delete(j);
    return ESP_OK;
}

static esp_err_t h_data_serve(httpd_req_t *req)
{
    const char *fp = req->uri + 6; /* Skip "/data/" */
    if (!*fp || strstr(fp, "..")) {
        return send_error_response(req, 404, "not found");
    }

    char decoded[PATH_MAX];
    if (!url_decode_component(fp, decoded, sizeof(decoded))) {
        return send_error_response(req, 404, "not found");
    }
    if (!decoded[0] || strstr(decoded, "..") || strchr(decoded, '/') || strchr(decoded, '\\')) {
        return send_error_response(req, 404, "not found");
    }

    char full[PATH_MAX];
    if (snprintf(full, sizeof(full), "%s/%s", BASE_PATH, decoded) >= (int)sizeof(full)) {
        return send_error_response(req, 404, "not found");
    }
    struct stat st;
    if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) {
        return send_error_response(req, 404, "not found");
    }
    return send_file_response(req, full);
}

static esp_err_t h_push_display(httpd_req_t *req)
{
    AppOperationGuard operation_guard;
    esp_err_t auth_err = require_machine_auth(req);
    if (auth_err != ESP_OK) {
        return auth_err;
    }
    if (req->content_len <= 0) {
        return send_error_response(req, 400, "no body");
    }
    if (req->content_len > PUSH_MAX_SIZE) {
        return send_error_response(req, 413, "image too large");
    }

    char *body = static_cast<char *>(malloc(req->content_len));
    if (!body) {
        return send_error_response(req, 500, "oom");
    }

    size_t left = req->content_len;
    size_t off  = 0;
    while (left > 0) {
        int ret = httpd_req_recv(req, body + off, left);
        if (ret <= 0) {
            free(body);
            return send_error_response(req, 500, "read err");
        }
        off += ret;
        left -= ret;
    }

    char ext[16] = {0};
    if (!detect_image_extension(body, off, ext, sizeof(ext))) {
        free(body);
        return send_error_response(req, 400, "unsupported image type");
    }

    char fname[64];
    snprintf(fname, sizeof(fname), "push_latest.%s", ext);

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", BASE_PATH, fname);

    hal_storage_prepare_photo_fs_access();
    hal_storage_lock();
    unlink(BASE_PATH "/push_latest.png");
    unlink(BASE_PATH "/push_latest.jpg");
    unlink(BASE_PATH "/push_latest.bmp");

    FILE *f = fopen(path, "wb");
    if (!f) {
        hal_storage_unlock();
        free(body);
        return send_error_response(req, 500, "write fail");
    }
    size_t written = fwrite(body, 1, off, f);
    fclose(f);
    hal_storage_unlock();
    free(body);

    if (written != off) {
        unlink(path);
        return send_error_response(req, 500, "short write");
    }

    photo_slideshow.displayPhotoByPath(path);
    strlcpy(g_dev_state.current_image, fname, sizeof(g_dev_state.current_image));

    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "status", "ok");
    cJSON_AddStringToObject(r, "name", fname);
    cJSON_AddStringToObject(r, "displaying", fname);
    cJSON_AddNumberToObject(r, "bytes", (double)off);
    send_json_response(req, r);
    cJSON_Delete(r);
    return ESP_OK;
}

static esp_err_t h_photos_display(httpd_req_t *req)
{
    AppOperationGuard operation_guard;
    char buf[256];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) return send_error_response(req, 400, "no body");
    buf[len] = 0;

    cJSON *j = cJSON_Parse(buf);
    if (!j) return send_error_response(req, 400, "bad json");

    cJSON *name_j = cJSON_GetObjectItem(j, "name");
    if (!cJSON_IsString(name_j) || !name_j->valuestring[0]) {
        cJSON_Delete(j);
        return send_error_response(req, 400, "name required");
    }
    const char *name = name_j->valuestring;

    /* Path traversal protection */
    if (strstr(name, "..") || strchr(name, '/') || strchr(name, '\\')) {
        cJSON_Delete(j);
        return send_error_response(req, 403, "invalid path");
    }

    /* Check whether the file exists */
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", BASE_PATH, name);
    struct stat st;
    hal_storage_prepare_photo_fs_access();
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        cJSON_Delete(j);
        return send_error_response(req, 404, "not found");
    }

    /* Call the local album to display the image */
    photo_slideshow.displayPhotoByPath(path);

    strlcpy(g_dev_state.current_image, name, sizeof(g_dev_state.current_image));

    cJSON_Delete(j);

    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "status", "ok");
    cJSON_AddStringToObject(r, "displaying", name);
    send_json_response(req, r);
    cJSON_Delete(r);
    return ESP_OK;
}

/* ==== Static files (SPA frontend) ==== */
static esp_err_t send_builtin_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
#if APP_ASSETS_USE_EMBEDDED
    size_t len = (size_t)(_binary_index_html_end - _binary_index_html_start);
    return httpd_resp_send(req, _binary_index_html_start, len);
#else
    return send_file_response(req, "/data/index.html");
#endif
}

static esp_err_t h_static_serve(httpd_req_t *req)
{
    const char *u = req->uri;
    if (strcmp(u, "/") == 0 || strcmp(u, "") == 0 || strcmp(u, "/index.html") == 0) {
        return send_builtin_index(req);
    }

    if (u[0] == '/') {
        u++;
    }

#if APP_ASSETS_USE_EMBEDDED
    return send_builtin_index(req);
#else
    char full[PATH_MAX];
    snprintf(full, sizeof(full), "%s/%s", BASE_PATH, u);
    struct stat st;
    if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) {
        return send_builtin_index(req);
    }
    return send_file_response(req, full);
#endif
}

static esp_err_t h_system_reset(httpd_req_t *req)
{
    app_manager_factory_reset_machine();
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "status", "ok");
    send_json_response(req, r);
    cJSON_Delete(r);
    return ESP_OK;
}

static void dhcp_set_captiveportal_url(void)
{
    // get the IP of the access point to redirect to
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (!netif) {
        ESP_LOGW(TAG, "AP netif not ready, skip captive portal url");
        return;
    }

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) {
        ESP_LOGW(TAG, "AP netif not ready, skip captive portal url");
        return;
    }

    char ip_addr[16];
    inet_ntoa_r(ip_info.ip.addr, ip_addr, 16);
    ESP_LOGI(TAG, "Set up softAP with IP: %s", ip_addr);

    // turn the IP into a URI
    static char captiveportal_uri[32];
    assert(captiveportal_uri && "Failed to allocate captiveportal_uri");
    strcpy(captiveportal_uri, "http://");
    strcat(captiveportal_uri, ip_addr);

    // set the DHCP option 114
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_stop(netif));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_option(netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI,
                                                         captiveportal_uri, strlen(captiveportal_uri)));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_start(netif));
}

// HTTP Error (404) Handler - Redirects all requests to the root page
esp_err_t http_404_error_handler(httpd_req_t *req, httpd_err_code_t err)
{
    // Set status
    httpd_resp_set_status(req, "303 See Other");
    // Redirect to the "/" root directory
    httpd_resp_set_hdr(req, "Location", "/");
    // iOS requires content in the response to detect a captive portal, simply redirecting is not sufficient.
    httpd_resp_send(req, "Redirect to the captive portal", HTTPD_RESP_USE_STRLEN);

    // ESP_LOGI(TAG, "Redirecting to root");
    return ESP_OK;
}

/* ==== Route table ==== */
static const httpd_uri_t routes[] = {
    {"/api/modes", HTTP_GET, h_get_modes},
    {"/api/wifi/scan", HTTP_GET, h_wifi_scan},
    {"/api/wifi/config", HTTP_POST, h_wifi_config},
    {"/api/wifi/status", HTTP_GET, h_wifi_status},
    {"/api/device/ready", HTTP_GET, h_device_ready},
    {"/api/hardware/diagnostics", HTTP_GET, h_hardware_diagnostics_get},
    {"/api/hardware/diagnostics", HTTP_POST, h_hardware_diagnostics_post},
    {"/api/wifi/disconnect", HTTP_POST, h_wifi_disconnect},
    {"/api/photos/list", HTTP_GET, h_photos_list},
    {"/api/photos/upload", HTTP_POST, h_photos_upload},
    {"/api/photos/delete", HTTP_DELETE, h_photos_delete},
    {"/api/storage", HTTP_GET, h_storage},
    {"/api/battery", HTTP_GET, h_battery},
    {"/api/mode/mode_1/config", HTTP_GET, h_mode_cfg_get},
    {"/api/mode/mode_1/config", HTTP_POST, h_mode_cfg_set},
    {"/api/mode/switch", HTTP_POST, h_mode_switch},
    {"/data/*", HTTP_GET, h_data_serve},
    {"/api/push/display", HTTP_POST, h_push_display},
    {"/api/photos/display", HTTP_POST, h_photos_display},
    {"/api/system/reset", HTTP_POST, h_system_reset},
    {"/", HTTP_GET, h_static_serve},

};

esp_err_t app_server_init(void)
{
    // Initialize device state from persisted settings
    strlcpy(g_dev_state.current_mode, hal.settings.current_mode, sizeof(g_dev_state.current_mode));
    if (hal.settings.current_mode[0]) {
        strlcpy(g_dev_state.requested_mode, hal.settings.current_mode, sizeof(g_dev_state.requested_mode));
    }

    /* WiFi events — additional listener, does not own the WiFi lifecycle. */
    WiFi.onEvent([](WiFiEvent ev, void *data) {
        switch (ev) {
            case WiFiEvent::STA_GOT_IP:
                ESP_LOGI(TAG, ">>> STA GOT IP");
                hal.statusEventSend(OPERATION_EVENT_STARTUP_SUCCESS);
                break;
            case WiFiEvent::AP_STA_CONNECTED:
                ESP_LOGI(TAG, ">>> AP: station connected");
                break;
            case WiFiEvent::AP_STA_DISCONNECTED:
                ESP_LOGI(TAG, ">>> AP: station leaved");
                break;
            default:
                break;
        }
    });

    esp_log_level_set("wifi", ESP_LOG_ERROR);
    esp_log_level_set("httpd", ESP_LOG_ERROR);
    esp_log_level_set("httpd_uri", ESP_LOG_ERROR);
    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);
    esp_log_level_set("httpd_parse", ESP_LOG_ERROR);
    esp_log_level_set("example_dns_redirect_server", ESP_LOG_ERROR);

    dhcp_set_captiveportal_url();

    /* HTTP server */
    httpd_config_t hc   = HTTPD_DEFAULT_CONFIG();
    hc.max_uri_handlers = 20;
    hc.stack_size       = 1024 * 20;
    hc.lru_purge_enable = true;
    hc.uri_match_fn     = httpd_uri_match_wildcard;
    hc.max_req_hdr_len  = 2048;
    hc.max_open_sockets = 13;
    hc.lru_purge_enable = true;
    esp_err_t err       = httpd_start(&g_srv, &hc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd start fail");
        return err;
    }

    for (unsigned i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(g_srv, &routes[i]);
    }
    httpd_register_err_handler(g_srv, HTTPD_404_NOT_FOUND, http_404_error_handler);

    /* Initialize mDNS once at boot so the device is reachable as
     * <device_name>.local on any active interface. Previously this only
     * ran inside the AP_STA_CONNECTED event handler, which meant mDNS was
     * only initialized when a station joined the device's softAP — so in
     * STA-only operation (or before any phone ever joined the AP)
     * <device_name>.local never resolved on the LAN. The mdns component
     * subscribes to netif up/down events itself; calling mdns_init() once
     * here is enough for both STA and AP traffic, and the hostname is
     * already re-applied on rename from h_wifi_config. */
    {
        esp_err_t merr = mdns_init();
        if (merr != ESP_OK) {
            ESP_LOGW(TAG, "mdns_init failed: %s", esp_err_to_name(merr));
        } else {
            char device_name[sizeof(hal.settings.device_name)] = {0};
            hal.settingsLock();
            cstring_copy(device_name, hal.settings.device_name, sizeof(device_name));
            hal.settingsUnlock();
            if (!device_name[0]) {
                cstring_copy(device_name, "papercolor", sizeof(device_name));
            }
            mdns_hostname_set(device_name);
            mdns_instance_name_set("PaperColor - Config Panel");
            /* Advertise an HTTP service so Bonjour-aware clients (Finder's
             * Network sidebar, dns-sd -B _http._tcp, etc.) discover the
             * device without needing to know its hostname. */
            mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
        }
    }

    /* ==== Captive Portal Handlers ==== */
    // Reference: https://github.com/espressif/esp-idf/tree/v5.5/examples/protocols/http_server/captive_portal

    // Start the DNS server that will redirect all queries to the softAP IP
    dns_server_config_t config = DNS_SERVER_CONFIG_SINGLE("*" /* all A queries */, "WIFI_AP_DEF" /* softAP netif ID */);
    start_dns_server(&config);

    ESP_LOGI(TAG, "papercolor server ready, %u routes", sizeof(routes) / sizeof(routes[0]));
    return ESP_OK;
}

esp_err_t app_server_stop(void)
{
    if (g_srv) {
        httpd_stop(g_srv);
        g_srv = NULL;
    }
    return ESP_OK;
}

void app_server_sync_mode(const char *mode_id)
{
    strlcpy(g_dev_state.current_mode, mode_id, sizeof(g_dev_state.current_mode));
    if (mode_id && mode_id[0]) {
        strlcpy(g_dev_state.requested_mode, mode_id, sizeof(g_dev_state.requested_mode));
    } else {
        g_dev_state.requested_mode[0] = 0;
    }
}

device_state_t app_server_get_state(void)
{
    device_state_t s = {};
    s.wifi_connected = WiFi.isConnected();
    if (s.wifi_connected) {
        std::string ip = WiFi.localIP();
        strlcpy(s.ip_address, ip.c_str(), sizeof(s.ip_address));
    }
    strlcpy(s.current_mode, g_dev_state.current_mode, sizeof(s.current_mode));
    strlcpy(s.requested_mode, g_dev_state.requested_mode, sizeof(s.requested_mode));
    strlcpy(s.current_image, g_dev_state.current_image, sizeof(s.current_image));
    strlcpy(s.connected_ssid, WiFi.SSID().c_str(), sizeof(s.connected_ssid));
    strlcpy(s.conn_err, g_dev_state.conn_err, sizeof(s.conn_err));
    return s;
}

mode1_config_t app_server_get_mode1_config(void)
{
    mode1_config_t c;
    strlcpy(c.orientation, hal.settings.rotation == 0 ? "landscape" : "portrait", sizeof(c.orientation));
    c.auto_slideshow   = hal.settings.auto_slideshow;
    c.interval_minutes = hal.settings.interval_minutes;
    c.low_power_mode   = hal.settings.low_power_mode;
    return c;
}
