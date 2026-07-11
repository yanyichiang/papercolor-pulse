/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal_ezdata.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>
#include "hal/hal.h"
#include "apps/ezdata_photo_push/ezdata_blank_image.h"
#include "hal/utils/daemon_control/daemon_control.h"
#include "mqtt_client.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "freertos/event_groups.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstdio>

#define DEVICE_ADD_DATA    (100)
#define DEVICE_MODIFY_DATA (101)
#define DEVICE_QUERY_DATA  (103)
#define DEVICE_INFO_DATA   (104)
#define USER_MODIFY_DATA   (107)
#define USER_ADD_DATA      (109)
#define REQUEST_TYPE_PING  (255)

class EzdataDaemonControl_t : public DaemonControl_t {
public:
    bool is_register_ok             = false;
    bool is_wifi_connected          = false;
    bool is_ezdata_connected        = false;
    std::string current_wifi_status = "not config";
};

static EzdataDaemonControl_t *_daemon_control = nullptr;
static volatile bool _daemon_should_stop      = false;  // Stop signal
static volatile bool _daemon_cleanup_done     = true;
static TaskHandle_t _daemon_task_handle       = nullptr;  // Save task handle

static std::string _ezdata_mqtt_topic_up;
static std::string _ezdata_mqtt_topic_down;
static const char *_ezdata_mqtt_host         = "uiflow2.m5stack.com";
static esp_mqtt_client_handle_t _mqtt_client = nullptr;
static EventGroupHandle_t _mqtt_event_group  = nullptr;
static bool _mqtt_is_connected               = false;

uint32_t _heart_beat_time_count = 0;
uint32_t _time_count            = 0;

static const int MQTT_CONNECTED_BIT    = BIT0;
static const int MQTT_DISCONNECTED_BIT = BIT1;

static std::string _device_token;
static bool _query_data   = false;
static int _post_interval = 5;
static const char *TAG    = "hal_ezdata";

std::vector<std::string> g_ezdata_photo_link_list;
static EventGroupHandle_t g_ezdata_event_group = nullptr;
#define EZDATA_PHOTO_LIST_READY_BIT    BIT0
#define EZDATA_NEW_PHOTO_AVAILABLE_BIT BIT1

static std::string g_latest_photo_url;
static uint32_t g_latest_photo_ready_at_ms = 0;
static std::string g_image_record_url;
static uint8_t g_image_record_fingerprint        = 0;
static uint8_t g_image_record_stored_fingerprint = 0;
static bool g_has_image_record                   = false;
static bool g_image_record_updated               = false;

static constexpr uint16_t MAX_PHOTOS = 200;

/* ── Collect the HTTP response body with std::string ── */
struct HttpResponseBuffer {
    std::string body;
};

static inline uint32_t millis()
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

/* ── Read cJSON fields ── */
static inline const char *_cjson_get_string(cJSON *obj, const char *key, const char *def = "")
{
    cJSON *item = cJSON_GetObjectItem(obj, key);
    return (item && cJSON_IsString(item)) ? item->valuestring : def;
}

static inline int _cjson_get_int(cJSON *obj, const char *key, int def = 0)
{
    cJSON *item = cJSON_GetObjectItem(obj, key);
    return (item && cJSON_IsNumber(item)) ? item->valueint : def;
}

static inline bool _cjson_get_bool(cJSON *obj, const char *key, bool def = false)
{
    cJSON *item = cJSON_GetObjectItem(obj, key);
    if (!item) return def;
    if (cJSON_IsBool(item)) return cJSON_IsTrue(item);
    if (cJSON_IsNumber(item)) return item->valueint != 0;
    return def;
}

static inline char _ascii_lower(char ch)
{
    return (ch >= 'A' && ch <= 'Z') ? (char)(ch - 'A' + 'a') : ch;
}

static inline bool _ext_equals_ignore_case(const char *dot, const char *ext)
{
    if (!dot || !ext) return false;
    while (*dot && *ext) {
        if (_ascii_lower(*dot) != _ascii_lower(*ext)) return false;
        ++dot;
        ++ext;
    }
    return *dot == '\0' && *ext == '\0';
}

static inline const char *_find_last_dot(const char *value)
{
    if (!value) return nullptr;
    const char *last = nullptr;
    while (*value) {
        if (*value == '.') last = value;
        ++value;
    }
    return last;
}

static inline bool _is_supported_image_ext(const char *dot)
{
    return _ext_equals_ignore_case(dot, ".jpg") || _ext_equals_ignore_case(dot, ".jpeg") ||
           _ext_equals_ignore_case(dot, ".bmp") || _ext_equals_ignore_case(dot, ".png");
}

static inline bool _is_supported_image_url(const char *value)
{
    const char *dot = _find_last_dot(value);
    return dot && _is_supported_image_ext(dot);
}

static inline uint8_t timestamp_fingerprint(const char *str)
{
    uint8_t hash = 0xA5;
    while (str && *str) {
        hash ^= (uint8_t)(*str);
        hash = (uint8_t)((hash << 1) | (hash >> 7));
        hash = (uint8_t)(hash + 0x37);
        ++str;
    }
    return hash;
}

static inline uint8_t image_record_fingerprint(const char *update_time, const char *url)
{
    return (update_time && update_time[0]) ? timestamp_fingerprint(update_time) : timestamp_fingerprint(url);
}

static esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    auto *buf = static_cast<HttpResponseBuffer *>(evt->user_data);
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (buf && evt->data_len > 0) {
                buf->body.append(static_cast<const char *>(evt->data), evt->data_len);
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

static bool _get_device_token()
{
    _device_token.clear();
    ESP_LOGI(TAG, "try get device token");

    const char *api_url = "https://ezdata2.m5stack.com/api/v2/device/registerMac";
    ESP_LOGI(TAG, "api url: %s", api_url);

    /* ====== Build JSON request body ====== */
    std::string json_content;
    {
        uint8_t mac[6] = {};
        hal.getDeviceMac(mac);

        char mac_str[13] = {};
        snprintf(mac_str, sizeof(mac_str), "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "deviceType", "papercolor");
        cJSON_AddStringToObject(root, "mac", mac_str);

        char *raw    = cJSON_PrintUnformatted(root);
        json_content = raw;
        free(raw);  // Memory allocated by cJSON must be freed manually
        cJSON_Delete(root);
    }
    ESP_LOGI(TAG, "post json:\n%s", json_content.c_str());

    /* ====== Configure HTTP client ====== */
    HttpResponseBuffer resp_buf;

    esp_http_client_config_t config = {};
    config.url                      = api_url;
    config.method                   = HTTP_METHOD_POST;
    config.event_handler            = _http_event_handler;
    config.user_data                = &resp_buf;
    config.transport_type           = HTTP_TRANSPORT_OVER_SSL;
    config.crt_bundle_attach        = esp_crt_bundle_attach;  // Use the built-in ESP-IDF CA certificate bundle

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "failed to init http client");
        return false;
    }

    /* ====== Set headers & request body, then send ====== */
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, json_content.c_str(), static_cast<int>(json_content.size()));

    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP POST failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return false;
    }

    /* ====== Read status code ====== */
    int response_code = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "get response code: %d", response_code);
    esp_http_client_cleanup(client);  // Release promptly

    if (response_code <= 0) {
        return false;
    }

    ESP_LOGI(TAG, "get response:\n%s", resp_buf.body.c_str());

    if (response_code != 200) {
        return false;
    }

    /* ====== Parse response JSON ====== */
    cJSON *doc = cJSON_Parse(resp_buf.body.c_str());
    if (!doc) {
        ESP_LOGE(TAG, "json parse failed");
        return false;
    }

    bool ok = false;
    do {
        cJSON *code_item = cJSON_GetObjectItem(doc, "code");
        if (!code_item || !cJSON_IsNumber(code_item) || code_item->valueint != 200) {
            ESP_LOGE(TAG, "invalid code: %d", code_item ? code_item->valueint : -1);
            break;
        }

        cJSON *data_item = cJSON_GetObjectItem(doc, "data");
        if (!data_item || !cJSON_IsString(data_item) || !data_item->valuestring[0]) {
            ESP_LOGE(TAG, "empty token");
            break;
        }

        _device_token = data_item->valuestring;
        ESP_LOGI(TAG, "get token: %s", _device_token.c_str());
        hal.device_token = _device_token;
        ok               = true;
    } while (false);

    cJSON_Delete(doc);
    return ok;
}

static void ws_send_json(uint8_t request_type, const char *name = "")
{
    char val_str[16];
    cJSON *root = cJSON_CreateObject();

    /* ====== device_token ====== */
    cJSON_AddStringToObject(root, "deviceToken", hal.device_token.c_str());

    /* ====== body ====== */
    bool is_ping = (request_type == REQUEST_TYPE_PING);

    if (!is_ping) {
        cJSON *body = cJSON_CreateObject();
        cJSON_AddNumberToObject(body, "requestType", request_type);

        if (name && name[0] != '\0') {
            cJSON_AddStringToObject(body, "name", name);

            if (strcmp(name, "temperature") == 0) {
                snprintf(val_str, sizeof(val_str), "%.2f", hal.temperature);
                cJSON_AddRawToObject(body, "value", val_str);
            } else if (strcmp(name, "humidity") == 0) {
                snprintf(val_str, sizeof(val_str), "%.2f", hal.humidity);
                cJSON_AddRawToObject(body, "value", val_str);
            }
        }

        cJSON_AddItemToObject(root, "body", body);
    } else {
        cJSON_AddStringToObject(root, "body", "ping");
    }

    /* ====== Serialize & publish ====== */
    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str) {
        ESP_LOGI(TAG, "[Local] post %d: %s", request_type, json_str);

        esp_mqtt_client_publish(_mqtt_client, _ezdata_mqtt_topic_up.c_str(), json_str,
                                0,  // len=0 -> auto strlen
                                0,  // QoS 0
                                0);

        free(json_str);
    }
    cJSON_Delete(root);
}

static bool _upload_device_image_file()
{
    if (_device_token.empty()) {
        ESP_LOGW(TAG, "skip image upload: device token empty");
        return false;
    }

    const char *url          = "https://ezdata2.m5stack.com/api/v2/device/uploadDeviceFile";
    const char *boundary     = "----PaperColorEzdataBoundary7MA4YWxkTrZu0gW";
    const char *filename     = "image.jpg";
    const char *content_type = "image/jpeg";

    std::string header;
    header.reserve(768);
    header += "--";
    header += boundary;
    header += "\r\nContent-Disposition: form-data; name=\"deviceToken\"\r\n\r\n";
    header += _device_token;
    header += "\r\n--";
    header += boundary;
    header += "\r\nContent-Disposition: form-data; name=\"name\"\r\n\r\nimage";
    header += "\r\n--";
    header += boundary;
    header += "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"";
    header += filename;
    header += "\"\r\nContent-Type: ";
    header += content_type;
    header += "\r\n\r\n";

    std::string footer;
    footer.reserve(64);
    footer += "\r\n--";
    footer += boundary;
    footer += "--\r\n";

    const size_t total_len = header.size() + sizeof(s_blank_image_jpeg) + footer.size();
    uint8_t *body          = static_cast<uint8_t *>(malloc(total_len));
    if (!body) {
        ESP_LOGE(TAG, "malloc upload body failed: %u", (unsigned)total_len);
        return false;
    }

    size_t offset = 0;
    memcpy(body + offset, header.data(), header.size());
    offset += header.size();
    memcpy(body + offset, s_blank_image_jpeg, sizeof(s_blank_image_jpeg));
    offset += sizeof(s_blank_image_jpeg);
    memcpy(body + offset, footer.data(), footer.size());
    offset += footer.size();

    HttpResponseBuffer resp_buf;
    esp_http_client_config_t config = {};
    config.url                      = url;
    config.method                   = HTTP_METHOD_POST;
    config.event_handler            = _http_event_handler;
    config.user_data                = &resp_buf;
    config.transport_type           = HTTP_TRANSPORT_OVER_SSL;
    config.crt_bundle_attach        = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(body);
        ESP_LOGE(TAG, "failed to init upload client");
        return false;
    }

    char content_type_header[128];
    snprintf(content_type_header, sizeof(content_type_header), "multipart/form-data; boundary=%s", boundary);
    esp_http_client_set_header(client, "Content-Type", content_type_header);
    esp_http_client_set_post_field(client, reinterpret_cast<const char *>(body), static_cast<int>(offset));

    ESP_LOGI(TAG, "uploading image file as %s", filename);
    esp_err_t err = esp_http_client_perform(client);
    free(body);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "image upload failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return false;
    }

    int response_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    ESP_LOGI(TAG, "image upload response code: %d", response_code);
    if (!resp_buf.body.empty()) {
        ESP_LOGI(TAG, "image upload response: %s", resp_buf.body.c_str());
    }

    if (response_code != 200) {
        return false;
    }

    cJSON *doc = cJSON_Parse(resp_buf.body.c_str());
    if (!doc) {
        ESP_LOGE(TAG, "image upload response parse failed");
        return false;
    }

    cJSON *code_item = cJSON_GetObjectItem(doc, "code");
    bool ok          = code_item && cJSON_IsNumber(code_item) && code_item->valueint == 200;
    cJSON_Delete(doc);
    if (!ok) {
        ESP_LOGE(TAG, "image upload business code invalid");
    }
    return ok;
}

static void onResponse(cJSON *response_doc)
{
    int cmd     = _cjson_get_int(response_doc, "cmd");
    int code    = _cjson_get_int(response_doc, "code");
    cJSON *body = cJSON_GetObjectItem(response_doc, "body");

    if (!_query_data) {
        /* ====== Query existing data fields ====== */
        if (cmd == DEVICE_QUERY_DATA && code == 200 && body && cJSON_IsArray(body)) {
            g_ezdata_photo_link_list.clear();
            bool found_image_record = false;
            std::string latest_image_url;
            uint8_t latest_image_fingerprint        = 0;
            std::vector<std::string> required_names = {"temperature", "humidity", "image"};
            std::set<std::string> existNames;

            cJSON *item = nullptr;
            cJSON_ArrayForEach(item, body)
            {
                const char *n           = _cjson_get_string(item, "name");
                const char *value       = _cjson_get_string(item, "value");
                const char *update_time = _cjson_get_string(item, "updateTime");
                if (n[0]) existNames.insert(n);
                if (_is_supported_image_url(value) && g_ezdata_photo_link_list.size() < MAX_PHOTOS) {
                    g_ezdata_photo_link_list.push_back(std::string(value));
                }
                if (n[0] && strcmp(n, "image") == 0 && _is_supported_image_url(value)) {
                    found_image_record       = true;
                    latest_image_url         = value;
                    latest_image_fingerprint = image_record_fingerprint(update_time, value);
                }
            }

            printf("existing names:\n");
            for (const auto &name : existNames) {
                printf(" - %s\n", name.c_str());
            }

            if (found_image_record) {
                bool had_image_record              = g_has_image_record;
                std::string previous_image_url     = g_image_record_url;
                uint8_t previous_image_fingerprint = g_image_record_fingerprint;

                bool image_updated = !had_image_record || latest_image_fingerprint != previous_image_fingerprint ||
                                     previous_image_url != latest_image_url;
                g_image_record_url         = latest_image_url;
                g_image_record_fingerprint = latest_image_fingerprint;
                g_has_image_record         = true;
                g_image_record_updated = image_updated && latest_image_fingerprint != g_image_record_stored_fingerprint;
                ESP_LOGI(TAG, "[EZData] Startup image record: fingerprint=%u, updated=%d, url=%s",
                         (unsigned)g_image_record_fingerprint, image_updated ? 1 : 0, g_image_record_url.c_str());
            }

            for (auto &name : required_names) {
                if (existNames.find(name) == existNames.end()) {
                    if (name == "image") {
                        ESP_LOGI(TAG, "[Local] %s not found, uploading image file", name.c_str());
                        _upload_device_image_file();
                    } else {
                        ESP_LOGI(TAG, "[Local] %s not found, adding default data", name.c_str());
                        ws_send_json(DEVICE_ADD_DATA, name.c_str());
                    }
                }
            }
            hal.sht40Read(&hal.temperature, &hal.humidity);
            ws_send_json(DEVICE_MODIFY_DATA, "temperature");
            ws_send_json(DEVICE_MODIFY_DATA, "humidity");
            xEventGroupClearBits(g_ezdata_event_group, EZDATA_PHOTO_LIST_READY_BIT);
            xEventGroupSetBits(g_ezdata_event_group, EZDATA_PHOTO_LIST_READY_BIT);
        }
        _query_data = true;
    } else {
        /* ====== Query existing data field updates ====== */
        // if (cmd == USER_MODIFY_DATA && code == 200 &&
        //     body && cJSON_IsObject(body))
        if (cmd == DEVICE_QUERY_DATA && code == 200 && body && cJSON_IsArray(body)) {
            g_ezdata_photo_link_list.clear();
            bool found_image_record = false;
            std::string latest_image_url;
            uint8_t latest_image_fingerprint = 0;
            cJSON *item                      = nullptr;
            cJSON_ArrayForEach(item, body)
            {
                if (g_ezdata_photo_link_list.size() >= MAX_PHOTOS) break;

                const char *name        = _cjson_get_string(item, "name");
                const char *value       = _cjson_get_string(item, "value");
                const char *update_time = _cjson_get_string(item, "updateTime");
                if (_is_supported_image_url(value)) {
                    g_ezdata_photo_link_list.push_back(std::string(value));
                    ESP_LOGD(TAG, "[EZData] Photo entry: link=\"%s\"", g_ezdata_photo_link_list.back().c_str());
                }
                if (name[0] && strcmp(name, "image") == 0 && _is_supported_image_url(value)) {
                    found_image_record       = true;
                    latest_image_url         = value;
                    latest_image_fingerprint = image_record_fingerprint(update_time, value);
                }
            }

            bool image_updated = false;
            if (found_image_record) {
                bool had_image_record              = g_has_image_record;
                std::string previous_image_url     = g_image_record_url;
                uint8_t previous_image_fingerprint = g_image_record_fingerprint;

                image_updated = !had_image_record || latest_image_fingerprint != previous_image_fingerprint ||
                                previous_image_url != latest_image_url;
                g_image_record_url         = latest_image_url;
                g_image_record_fingerprint = latest_image_fingerprint;
                g_has_image_record         = true;
                g_image_record_updated = image_updated && latest_image_fingerprint != g_image_record_stored_fingerprint;
                ESP_LOGI(TAG, "[EZData] Image record: fingerprint=%u, updated=%d, url=%s",
                         (unsigned)g_image_record_fingerprint, image_updated ? 1 : 0, g_image_record_url.c_str());
            } else {
                ESP_LOGW(TAG, "[EZData] Image record missing in query, keep last known state");
            }

            xEventGroupClearBits(g_ezdata_event_group, EZDATA_PHOTO_LIST_READY_BIT);
            xEventGroupSetBits(g_ezdata_event_group, EZDATA_PHOTO_LIST_READY_BIT);
            ESP_LOGI(TAG, "[EZData] Total photos found: %d / max %d", (int)g_ezdata_photo_link_list.size(), MAX_PHOTOS);
        } else if (cmd == USER_ADD_DATA && code == 200 && body && cJSON_IsObject(body)) {
            const char *value = _cjson_get_string(body, "value");
            if (_is_supported_image_url(value)) {
                g_latest_photo_url         = value;
                g_latest_photo_ready_at_ms = millis() + 100;
                xEventGroupClearBits(g_ezdata_event_group, EZDATA_NEW_PHOTO_AVAILABLE_BIT);
                ESP_LOGI(TAG, "[EZData] New photo queued: %s", g_latest_photo_url.c_str());
            }
        }
    }
}

static void _on_mqtt_msg(const char *payload, int payload_len)
{
    cJSON *doc = cJSON_ParseWithLength(payload, payload_len);
    if (!doc) {
        ESP_LOGW(TAG, "parse msg json failed");
        return;
    }
    onResponse(doc);
    cJSON_Delete(doc);
}

static void _mqtt_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    auto *event = static_cast<esp_mqtt_event_handle_t>(event_data);

    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            _mqtt_is_connected = true;
            ESP_LOGI(TAG, "mqtt client connected");
            xEventGroupSetBits(_mqtt_event_group, MQTT_CONNECTED_BIT);
            break;

        case MQTT_EVENT_DISCONNECTED:
            _mqtt_is_connected = false;
            ESP_LOGW(TAG, "mqtt client disconnected");
            xEventGroupSetBits(_mqtt_event_group, MQTT_DISCONNECTED_BIT);
            break;

        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(TAG, "mqtt subscribed, msg_id=%d", event->msg_id);
            break;

        case MQTT_EVENT_PUBLISHED:
            ESP_LOGI(TAG, "mqtt published, msg_id=%d", event->msg_id);
            break;

        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "mqtt error type: %d", event->error_handle->error_type);
            break;

        case MQTT_EVENT_DATA:
            // ESP_LOGI(TAG, "Data:  %.*s", event->data_len, event->data);
            _on_mqtt_msg(event->data, event->data_len);

            break;

        default:
            break;
    }
}

static bool _connect_ezdata_mqtt()
{
    bool ret = false;
    ESP_LOGI(TAG, "[ezdata] try connect ezdata mqtt..");

    /* ====== Build client_id ====== */
    std::string client_id = "ez";
    {
        uint8_t mac[6] = {};
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        char mac_str[13] = {};
        snprintf(mac_str, sizeof(mac_str), "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        client_id += mac_str;
    }
    client_id += "ez";

    ESP_LOGI(TAG, "[ezdata] username:  %s", _device_token.c_str());
    ESP_LOGI(TAG, "[ezdata] client_id: %s", client_id.c_str());

    /* ====== Create EventGroup ====== */
    if (!_mqtt_event_group) {
        _mqtt_event_group = xEventGroupCreate();
    }
    xEventGroupClearBits(_mqtt_event_group, MQTT_CONNECTED_BIT | MQTT_DISCONNECTED_BIT);

    /* ====== Configure & create MQTT client ====== */
    esp_mqtt_client_config_t mqtt_cfg = {};
    mqtt_cfg.broker.address.hostname  = _ezdata_mqtt_host;
    mqtt_cfg.broker.address.port      = 1883;
    mqtt_cfg.broker.address.transport = MQTT_TRANSPORT_OVER_TCP;
    mqtt_cfg.credentials.username     = _device_token.c_str();
    mqtt_cfg.credentials.client_id    = client_id.c_str();
    mqtt_cfg.buffer.out_size          = 1024;
    mqtt_cfg.buffer.size              = 4096 * 20;

    /* If it was already created before, destroy it first */
    if (_mqtt_client) {
        esp_mqtt_client_stop(_mqtt_client);
        esp_mqtt_client_destroy(_mqtt_client);
        _mqtt_client = nullptr;
    }

    _mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (!_mqtt_client) {
        ESP_LOGE(TAG, "failed to init mqtt client");
        return false;
    }

    /* ====== Register events & start ====== */
    esp_mqtt_client_register_event(_mqtt_client, static_cast<esp_mqtt_event_id_t>(ESP_EVENT_ANY_ID),
                                   _mqtt_event_handler, nullptr);
    esp_mqtt_client_start(_mqtt_client);

    /* ====== Wait for connection result (with retry) ====== */
    ESP_LOGI(TAG, "[ezdata] try connect mqtt..");

    while (true) {
        if (_daemon_should_stop) {
            ESP_LOGW(TAG, "mqtt connect aborted — daemon stopping");
            ret = false;
            break;
        }

        EventBits_t bits = xEventGroupWaitBits(_mqtt_event_group, MQTT_CONNECTED_BIT | MQTT_DISCONNECTED_BIT,
                                               pdTRUE,              // Clear automatically
                                               pdFALSE,             // Any bit is enough
                                               pdMS_TO_TICKS(5000)  // Timeout 5s
        );

        if (bits & MQTT_CONNECTED_BIT) {
            ESP_LOGI(TAG, "mqtt client connected");
            ret = true;
            break;
        }

        /* Connection failed or timed out -> retry */
        ESP_LOGW(TAG, "mqtt connect failed, retry in 1s");
        vTaskDelay(pdMS_TO_TICKS(1000));

        /* Restart the client to trigger reconnect */
        esp_mqtt_client_reconnect(_mqtt_client);
    }

    /* ====== Update shared state ====== */
    _daemon_control->Borrow();
    _daemon_control->is_ezdata_connected = ret;
    _daemon_control->Return();

    return ret;
}

/* ── Set topics & subscribe ── */
static bool _setup_ezdata_mqtt()
{
    /* ====== Get topic names ====== */
    _ezdata_mqtt_topic_up   = "$ezdata/" + hal.device_token + "/up";
    _ezdata_mqtt_topic_down = "$ezdata/" + hal.device_token + "/down";

    ESP_LOGI(TAG, "[ezdata] topic up:   %s", _ezdata_mqtt_topic_up.c_str());
    ESP_LOGI(TAG, "[ezdata] topic down: %s", _ezdata_mqtt_topic_down.c_str());

    /* ====== Subscribe down topic ====== */
    int msg_id = esp_mqtt_client_subscribe(_mqtt_client, _ezdata_mqtt_topic_down.c_str(),
                                           0);  // QoS 0
    if (msg_id < 0) {
        ESP_LOGE(TAG, "subscribe failed");
        return false;
    }
    ESP_LOGI(TAG, "[ezdata] subscribe msg_id=%d", msg_id);

    return true;
}

static void _upload_papercolor_temp_hum()
{
    hal.sht40Read(&hal.temperature, &hal.humidity);
    _time_count            = millis();
    _heart_beat_time_count = millis();
    ws_send_json(DEVICE_MODIFY_DATA, "temperature");
    ws_send_json(DEVICE_MODIFY_DATA, "humidity");
}

bool getImageFileLinkList()
{
    if (!g_ezdata_event_group) {
        return false;
    }

    xEventGroupClearBits(g_ezdata_event_group, EZDATA_PHOTO_LIST_READY_BIT);
    ws_send_json(DEVICE_QUERY_DATA, "");

    // Wait for the response event, timeout 5 seconds
    EventBits_t bits = xEventGroupWaitBits(g_ezdata_event_group,
                                           EZDATA_PHOTO_LIST_READY_BIT,  // Bits to wait for
                                           pdTRUE,                       // xClearOnExit: clear automatically on exit
                                           pdFALSE,                      // xWaitForAllBits: no need for all bits
                                           pdMS_TO_TICKS(5000)           // Timeout 5000ms
    );

    if (bits & EZDATA_PHOTO_LIST_READY_BIT) {
        return true;
    } else {
        return false;
    }
}

static void _ezdata_daemon(void *param)
{
    if (uxTaskGetStackHighWaterMark(NULL) < 1024) {
        ESP_LOGW(TAG, "Warning: daemon stack remaining is low: %d bytes", uxTaskGetStackHighWaterMark(NULL));
    }

    ESP_LOGI(TAG, "ezdata daemon start");

    size_t stack_left = uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI(TAG, "ezdata daemon task stack left: %d bytes", stack_left);

    _daemon_control      = new EzdataDaemonControl_t;
    g_ezdata_event_group = xEventGroupCreate();
    // Check token
    while (1) {
        if (_daemon_should_stop) {
            ESP_LOGI(TAG, "daemon stop requested before token ready");
            break;
        }
        if (!hal_wifi::WiFi.isConnected()) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        if (_get_device_token()) {
            break;
        }
        ESP_LOGE(TAG, "ezdata token invalid, retry in 10s");
        vTaskDelay(pdMS_TO_TICKS(10000));
    }

    if (_daemon_should_stop) {
        ESP_LOGI(TAG, "daemon stop requested, cleaning up...");
    } else {
        uint32_t _heart_beat_time_count = millis();

        while (1) {
            if (_daemon_should_stop) {
                ESP_LOGI(TAG, "daemon stop requested, cleaning up...");
                break;
            }

            if (!hal_wifi::WiFi.isConnected()) {
                if (_mqtt_is_connected) {
                    _mqtt_is_connected   = false;
                    hal.ezdata_connected = false;
                }
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }

            if (!_mqtt_is_connected) {
                _connect_ezdata_mqtt();
                _setup_ezdata_mqtt();
                if (!_mqtt_is_connected) {
                    vTaskDelay(pdMS_TO_TICKS(500));
                    continue;
                }

                ESP_LOGI(TAG, "login successful");
                hal.ezdata_connected = true;
                ws_send_json(DEVICE_QUERY_DATA, "");
            } else {
                if (_query_data) {
                    if (millis() - _time_count > _post_interval * 1000) {
                        _upload_papercolor_temp_hum();
                        getImageFileLinkList();
                    }
                    if (millis() - _heart_beat_time_count > 45000) {
                        ESP_LOGI(TAG, "[Local] send ping to keep connection alive");
                        _heart_beat_time_count = millis();
                    }
                }
            }
            vTaskDelay(10 / portTICK_PERIOD_MS);
        }
    }

    /* ====== Clean up resources ====== */
    ESP_LOGI(TAG, "daemon cleanup start");

    // Disconnect MQTT
    if (_mqtt_client) {
        esp_mqtt_client_stop(_mqtt_client);
        esp_mqtt_client_destroy(_mqtt_client);
        _mqtt_client = nullptr;
    }

    // Delete EventGroups
    if (_mqtt_event_group) {
        vEventGroupDelete(_mqtt_event_group);
        _mqtt_event_group = nullptr;
    }
    if (g_ezdata_event_group) {
        vEventGroupDelete(g_ezdata_event_group);
        g_ezdata_event_group = nullptr;
    }

    // Clean up daemon control
    if (_daemon_control) {
        delete _daemon_control;
        _daemon_control = nullptr;
    }

    // Clean up global state
    _mqtt_is_connected = false;
    _device_token.clear();
    hal.device_token.clear();
    hal.ezdata_connected = false;
    _query_data          = false;
    g_ezdata_photo_link_list.clear();
    _ezdata_mqtt_topic_up.clear();
    _ezdata_mqtt_topic_down.clear();

    ESP_LOGI(TAG, "daemon cleanup done, task exiting");
    _daemon_task_handle = nullptr;
    vTaskDelete(NULL);
}

void ezdata_init()
{
    ESP_LOGI(TAG, "ezdata init");

    hal.rx8130RamRead(3, &g_image_record_stored_fingerprint);
    g_image_record_fingerprint = g_image_record_stored_fingerprint;
    g_image_record_updated     = false;
    g_has_image_record         = false;
    g_image_record_url.clear();
    g_latest_photo_url.clear();
    g_latest_photo_ready_at_ms = 0;

    // If it was already initialized before, clean it up first
    if (_daemon_task_handle != nullptr) {
        ESP_LOGW(TAG, "ezdata already initialized, deinit first");
        ezdata_deinit();
        vTaskDelay(pdMS_TO_TICKS(100));  // Give cleanup a little time
    }

    // Check remaining main task stack space
    size_t main_stack_left = uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI(TAG, "main task stack left: %d bytes", main_stack_left);

    _daemon_should_stop = false;
    xTaskCreate(_ezdata_daemon, "ezdata", 8192, NULL, 15, &_daemon_task_handle);
}

void ezdata_deinit()
{
    ESP_LOGI(TAG, "ezdata deinit requested");

    if (_daemon_task_handle == nullptr) {
        ESP_LOGW(TAG, "ezdata not running, nothing to deinit");
        return;
    }

    // Send the stop signal — the daemon will clean up MQTT, EventGroups, and state by itself
    _daemon_should_stop = true;

    // Wait for the daemon to exit on its own (up to 10s, enough time for MQTT disconnect)
    int wait_ms = 10000;
    while (_daemon_task_handle != nullptr && wait_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(50));
        wait_ms -= 50;
    }

    if (_daemon_task_handle != nullptr) {
        // Extremely unexpected case: the daemon still has not exited after 10s -> force delete and clean leftovers
        // manually
        ESP_LOGE(TAG, "daemon cleanup timed out after 10s, forcing delete");
        vTaskDelete(_daemon_task_handle);
        _daemon_task_handle = nullptr;

        if (_mqtt_client) {
            esp_mqtt_client_stop(_mqtt_client);
            esp_mqtt_client_destroy(_mqtt_client);
            _mqtt_client = nullptr;
        }
        if (_mqtt_event_group) {
            vEventGroupDelete(_mqtt_event_group);
            _mqtt_event_group = nullptr;
        }
        if (g_ezdata_event_group) {
            vEventGroupDelete(g_ezdata_event_group);
            g_ezdata_event_group = nullptr;
        }
        if (_daemon_control) {
            delete _daemon_control;
            _daemon_control = nullptr;
        }
        _mqtt_is_connected = false;
        _device_token.clear();
        hal.device_token.clear();
        hal.ezdata_connected = false;
        _query_data          = false;
        g_ezdata_photo_link_list.clear();
        _ezdata_mqtt_topic_up.clear();
        _ezdata_mqtt_topic_down.clear();
    }

    _daemon_should_stop = false;
    ESP_LOGI(TAG, "ezdata deinit complete");
}

bool ezdata_is_connected()
{
    return _mqtt_is_connected && !_device_token.empty();
}

typedef struct {
    uint8_t *buf;     // Buffer allocated in PSRAM
    size_t len;       // Received data length
    size_t capacity;  // Current allocated size
    size_t max_size;  // Maximum allowed image size (prevent OOM)
    bool overflow;    // Whether the limit was exceeded
} PsramImageBuffer;

static esp_err_t _http_photo_event_handler(esp_http_client_event_t *evt)
{
    PsramImageBuffer *img = static_cast<PsramImageBuffer *>(evt->user_data);
    if (!img) return ESP_OK;

    switch (evt->event_id) {
        /* ---- Parse response headers and try pre-allocation ---- */
        case HTTP_EVENT_ON_HEADER:
            if (strcasecmp(evt->header_key, "Content-Length") == 0) {
                size_t content_len = (size_t)atol(evt->header_value);
                ESP_LOGI(TAG, "[Photo] Content-Length: %u", (unsigned)content_len);

                if (content_len > img->max_size) {
                    ESP_LOGE(TAG, "[Photo] Too large: %u > max %u", (unsigned)content_len, (unsigned)img->max_size);
                    img->overflow = true;
                    break;
                }
                // Pre-allocate once in PSRAM
                if (!img->buf) {
                    img->buf = (uint8_t *)heap_caps_malloc(content_len, MALLOC_CAP_SPIRAM);
                    if (img->buf) {
                        img->capacity = content_len;
                        ESP_LOGD(TAG, "[Photo] Pre-allocated %u bytes in PSRAM", (unsigned)content_len);
                    } else {
                        ESP_LOGE(TAG, "[Photo] PSRAM malloc(%u) failed", (unsigned)content_len);
                        img->overflow = true;
                    }
                }
            }
            break;

        /* ---- Receive data chunks and write into the PSRAM buffer ---- */
        case HTTP_EVENT_ON_DATA:
            if (img->overflow || evt->data_len == 0) break;

            // Check whether it would exceed the limit
            if (img->len + evt->data_len > img->max_size) {
                ESP_LOGE(TAG, "[Photo] Exceeded max_size during download");
                img->overflow = true;
                break;
            }

            // If not pre-allocated (chunked transfer without Content-Length), grow dynamically
            if (img->len + evt->data_len > img->capacity) {
                size_t new_cap = img->capacity == 0 ? (64 * 1024) : (img->capacity * 2);
                while (new_cap < img->len + evt->data_len) {
                    new_cap *= 2;
                }
                if (new_cap > img->max_size) {
                    new_cap = img->max_size;
                }

                uint8_t *new_buf = (uint8_t *)heap_caps_realloc(img->buf, new_cap, MALLOC_CAP_SPIRAM);
                if (!new_buf) {
                    ESP_LOGE(TAG, "[Photo] PSRAM realloc(%u) failed", (unsigned)new_cap);
                    img->overflow = true;
                    break;
                }
                img->buf      = new_buf;
                img->capacity = new_cap;
                ESP_LOGD(TAG, "[Photo] PSRAM buffer grown to %u bytes", (unsigned)new_cap);
            }

            memcpy(img->buf + img->len, evt->data, evt->data_len);
            img->len += evt->data_len;
            break;

        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGD(TAG, "[Photo] Disconnected, received %u bytes", (unsigned)(img ? img->len : 0));
            break;

        default:
            break;
    }
    return ESP_OK;
}

/**
 * @brief Download an image from g_ezdata_photo_link_list into PSRAM
 *
 * @param index      List index (wrapped automatically)
 * @param out_data   Output: image data allocated in PSRAM, caller must heap_caps_free()
 * @param out_len    Output: image data length
 * @param max_size   Maximum allowed image size in bytes, for example 2*1024*1024
 * @return esp_err_t
 */
esp_err_t ezdata_fetch_photo(int index, uint8_t **out_data, size_t *out_len, size_t max_size)
{
    /* ---------- Validate parameters ---------- */
    if (!out_data || !out_len) return ESP_ERR_INVALID_ARG;
    *out_data = nullptr;
    *out_len  = 0;

    if (g_ezdata_photo_link_list.empty()) {
        ESP_LOGE(TAG, "[Photo] List is empty");
        return ESP_ERR_NOT_FOUND;
    }

    /* ---------- Wrap index ---------- */
    int count              = (int)g_ezdata_photo_link_list.size();
    int actual             = ((index % count) + count) % count;
    const std::string &url = g_ezdata_photo_link_list[actual];

    ESP_LOGI(TAG, "[Photo] Fetching [%d]->[%d/%d]: %s", index, actual, count, url.c_str());

    /* ---------- Initialize PSRAM buffer ---------- */
    PsramImageBuffer img_buf = {};
    img_buf.max_size         = max_size > 0 ? max_size : (2 * 1024 * 1024);  // Default 2MB

    /* ---------- HTTP configuration ---------- */
    esp_http_client_config_t config = {};
    config.url                      = url.c_str();
    config.event_handler            = _http_photo_event_handler;
    config.user_data                = &img_buf;
    config.timeout_ms               = 30000;     // Allow a longer timeout for large images
    config.buffer_size              = 8 * 1024;  // Receive chunk size 8KB
    config.buffer_size_tx           = 1024;
    config.crt_bundle_attach        = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "[Photo] HTTP client init failed");
        return ESP_FAIL;
    }

    /* ---------- Execute request ---------- */
    esp_err_t err = esp_http_client_perform(client);
    int status    = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    /* ---------- Error handling ---------- */
    if (err != ESP_OK || img_buf.overflow) {
        ESP_LOGE(TAG, "[Photo] Download failed: err=%s overflow=%d", esp_err_to_name(err), img_buf.overflow);
        if (img_buf.buf) heap_caps_free(img_buf.buf);
        return err != ESP_OK ? err : ESP_ERR_NO_MEM;
    }

    if (status != 200) {
        ESP_LOGE(TAG, "[Photo] HTTP status %d", status);
        if (img_buf.buf) heap_caps_free(img_buf.buf);
        return ESP_FAIL;
    }

    if (img_buf.len == 0) {
        ESP_LOGE(TAG, "[Photo] Empty body");
        if (img_buf.buf) heap_caps_free(img_buf.buf);
        return ESP_ERR_NOT_FOUND;
    }

    /* ---------- Shrink extra memory if needed (optional) ---------- */
    if (img_buf.capacity > img_buf.len + 1024) {
        uint8_t *shrunk = (uint8_t *)heap_caps_realloc(img_buf.buf, img_buf.len, MALLOC_CAP_SPIRAM);
        if (shrunk) {
            img_buf.buf      = shrunk;
            img_buf.capacity = img_buf.len;
        }
    }

    /* ---------- Output ---------- */
    *out_data = img_buf.buf;
    *out_len  = img_buf.len;

    ESP_LOGI(TAG, "[Photo] [%d] OK, %u bytes in PSRAM (free PSRAM: %u KB)", actual, (unsigned)img_buf.len,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));

    return ESP_OK;
}

esp_err_t ezdata_fetch_photo_by_url(const char *url, uint8_t **out_data, size_t *out_len, size_t max_size)
{
    if (!url || !out_data || !out_len) return ESP_ERR_INVALID_ARG;
    *out_data = nullptr;
    *out_len  = 0;

    if (strlen(url) == 0) {
        ESP_LOGE(TAG, "[Photo] Empty URL");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "[Photo] Fetching by URL: %s", url);

    PsramImageBuffer img_buf = {};
    img_buf.max_size         = max_size > 0 ? max_size : (2 * 1024 * 1024);

    esp_http_client_config_t config = {};
    config.url                      = url;
    config.event_handler            = _http_photo_event_handler;
    config.user_data                = &img_buf;
    config.timeout_ms               = 30000;
    config.buffer_size              = 8 * 1024;
    config.buffer_size_tx           = 1024;
    config.crt_bundle_attach        = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "[Photo] HTTP client init failed");
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status    = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || img_buf.overflow) {
        ESP_LOGE(TAG, "[Photo] Download failed: err=%s overflow=%d", esp_err_to_name(err), img_buf.overflow);
        if (img_buf.buf) heap_caps_free(img_buf.buf);
        return err != ESP_OK ? err : ESP_ERR_NO_MEM;
    }

    if (status != 200) {
        ESP_LOGE(TAG, "[Photo] HTTP status %d", status);
        if (img_buf.buf) heap_caps_free(img_buf.buf);
        return ESP_FAIL;
    }

    if (img_buf.len == 0) {
        ESP_LOGE(TAG, "[Photo] Empty body");
        if (img_buf.buf) heap_caps_free(img_buf.buf);
        return ESP_ERR_NOT_FOUND;
    }

    if (img_buf.capacity > img_buf.len + 1024) {
        uint8_t *shrunk = (uint8_t *)heap_caps_realloc(img_buf.buf, img_buf.len, MALLOC_CAP_SPIRAM);
        if (shrunk) {
            img_buf.buf      = shrunk;
            img_buf.capacity = img_buf.len;
        }
    }

    *out_data = img_buf.buf;
    *out_len  = img_buf.len;

    ESP_LOGI(TAG, "[Photo] Download OK, %u bytes in PSRAM", (unsigned)img_buf.len);
    return ESP_OK;
}

size_t ezdata_get_photo_count()
{
    return g_ezdata_photo_link_list.size();
}

const char *ezdata_get_photo_url(size_t index)
{
    if (index >= g_ezdata_photo_link_list.size()) return nullptr;
    return g_ezdata_photo_link_list[index].c_str();
}

bool ezdata_has_new_photo()
{
    if (g_latest_photo_url.empty()) return false;
    if ((int32_t)(millis() - g_latest_photo_ready_at_ms) < 0) return false;
    xEventGroupSetBits(g_ezdata_event_group, EZDATA_NEW_PHOTO_AVAILABLE_BIT);
    return true;
}

const char *ezdata_get_new_photo_url()
{
    return g_latest_photo_url.empty() ? nullptr : g_latest_photo_url.c_str();
}

void ezdata_clear_new_photo_flag()
{
    g_latest_photo_url.clear();
    g_latest_photo_ready_at_ms = 0;
    xEventGroupClearBits(g_ezdata_event_group, EZDATA_NEW_PHOTO_AVAILABLE_BIT);
}

bool ezdata_has_image_record()
{
    return g_has_image_record;
}

const char *ezdata_get_image_record_url()
{
    return g_has_image_record ? g_image_record_url.c_str() : nullptr;
}

uint8_t ezdata_get_image_record_fingerprint()
{
    return g_image_record_fingerprint;
}

void ezdata_mark_image_record_persisted()
{
    g_image_record_stored_fingerprint = g_image_record_fingerprint;
    g_image_record_updated            = false;
}

bool ezdata_take_image_update_flag()
{
    bool updated           = g_image_record_updated;
    g_image_record_updated = false;
    return updated;
}
