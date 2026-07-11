#include "scheduled_push.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <vector>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mbedtls/sha256.h"
#include "nvs.h"

#include "apps/local_photo_slideshow/local_photo_slideshow.h"
#include "apps/scheduled_push/pulse_page_policy.h"
#include "apps/scheduled_push/scheduled_push_policy.h"
#include "hal/hal.h"
#include "hal/storage/hal_storage.h"

namespace scheduled_push {
namespace {

constexpr const char* TAG                   = "ScheduledPush";
constexpr const char* CACHE_DIR             = "/data/.papercolor";
constexpr const char* MANIFEST_PATH         = "/data/.papercolor/manifest.json";
constexpr const char* MANIFEST_BACKUP_PATH  = "/data/.papercolor/manifest.bak";
constexpr const char* MANIFEST_TEMP_PATH    = "/data/.papercolor/manifest.tmp";
constexpr const char* VOICE_OUTBOX_DIR      = "/data/.papercolor/voice-outbox";
constexpr const char* VOICE_ARCHIVE_DIR     = "/data/.papercolor/voice-archive";
constexpr size_t MAX_MANIFEST_BYTES         = 64 * 1024;
constexpr size_t MAX_ASSET_BYTES            = 768 * 1024;
constexpr size_t MAX_JOBS                   = 16;
constexpr size_t MAX_CACHE_MANIFEST_BYTES   = 4 * 1024 * 1024;
constexpr int64_t MIN_VALID_EPOCH           = 1735689600;  // 2025-01-01 UTC
constexpr int64_t DISPLAY_WAKE_LEAD_SECONDS = 0;
constexpr int64_t DISPLAY_WAKE_WAIT_SECONDS = 65;
constexpr int64_t RETRY_SECONDS             = 5 * 60;

struct Job {
    std::string id;
    std::string asset_path;
    std::string asset_sha256;
    uint32_t asset_size = 0;
    int64_t display_at  = 0;
    int64_t expires_at  = 0;
    bool displayed      = false;
};

struct PulsePage {
    uint8_t index = 0;
    std::string name;
    std::string asset_path;
    std::string asset_sha256;
    uint32_t asset_size = 0;
};

struct Manifest {
    int protocol_version = 1;
    int64_t server_time  = 0;
    int64_t next_sync_at = 0;
    uint64_t version     = 0;
    std::vector<Job> jobs;
    std::vector<PulsePage> pulse_pages;
};

struct MemoryResponse {
    std::vector<uint8_t> body;
    size_t limit  = 0;
    bool overflow = false;
};

Manifest g_manifest;
bool g_manifest_loaded             = false;
uint32_t g_next_sync_ms            = 0;
uint32_t g_next_display_attempt_ms = 0;
size_t g_pulse_page_index          = 0;

uint32_t monotonic_ms()
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
}

void defer_periodic_retry(uint32_t now_ms)
{
    g_next_sync_ms = now_ms + static_cast<uint32_t>(RETRY_SECONDS * 1000);
}

void defer_display_retry(uint32_t now_ms)
{
    g_next_display_attempt_ms = now_ms + static_cast<uint32_t>(RETRY_SECONDS * 1000);
}

bool is_hex_sha256(const std::string& value)
{
    if (value.size() != 64) return false;
    for (char ch : value) {
        bool digit = ch >= '0' && ch <= '9';
        bool lower = ch >= 'a' && ch <= 'f';
        bool upper = ch >= 'A' && ch <= 'F';
        if (!(digit || lower || upper)) return false;
    }
    return true;
}

std::string lower_ascii(std::string value)
{
    for (char& ch : value) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    return value;
}

std::string asset_local_path(const std::string& sha256)
{
    return std::string(CACHE_DIR) + "/" + lower_ascii(sha256) + ".png";
}

std::string gateway_url(const std::string& path)
{
    std::string base = hal.settings.gateway_url;
    while (!base.empty() && base.back() == '/') base.pop_back();
    if (path.empty() || path.front() == '/') return base + path;
    return base + "/" + path;
}

std::string device_id()
{
    uint8_t mac[6] = {};
    hal.getDeviceMac(mac);
    char value[32];
    snprintf(value, sizeof(value), "papercolor-%02x%02x%02x", mac[3], mac[4], mac[5]);
    return value;
}

bool ensure_cache_dir()
{
    hal_storage_prepare_photo_fs_access();
    hal_storage_lock();
    int result = mkdir(CACHE_DIR, 0777);
    bool ok    = result == 0 || errno == EEXIST;
    hal_storage_unlock();
    if (!ok) ESP_LOGE(TAG, "failed to create cache directory: errno=%d", errno);
    return ok;
}

esp_err_t memory_event_handler(esp_http_client_event_t* event)
{
    if (event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0) return ESP_OK;
    auto* response = static_cast<MemoryResponse*>(event->user_data);
    if (!response || response->overflow) return ESP_FAIL;
    size_t incoming = static_cast<size_t>(event->data_len);
    if (response->body.size() + incoming > response->limit) {
        response->overflow = true;
        return ESP_FAIL;
    }
    const auto* begin = static_cast<const uint8_t*>(event->data);
    response->body.insert(response->body.end(), begin, begin + incoming);
    return ESP_OK;
}

bool set_device_authorization(esp_http_client_handle_t client)
{
    if (!client || !hal.settings.gateway_token[0]) return false;
    std::string value = "Bearer ";
    value += hal.settings.gateway_token;
    return esp_http_client_set_header(client, "Authorization", value.c_str()) == ESP_OK;
}

bool http_json_request(const std::string& url, esp_http_client_method_t method, const char* request_body,
                       std::vector<uint8_t>* response_body, int* status_code)
{
    MemoryResponse response;
    response.limit = MAX_MANIFEST_BYTES;
    if (response_body) response.body.reserve(4096);

    esp_http_client_config_t config = {};
    config.url                      = url.c_str();
    config.timeout_ms               = 15000;
    config.event_handler            = memory_event_handler;
    config.user_data                = &response;
    config.disable_auto_redirect    = true;
    config.crt_bundle_attach        = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return false;
    esp_http_client_set_method(client, method);
    if (!set_device_authorization(client)) {
        esp_http_client_cleanup(client);
        return false;
    }
    esp_http_client_set_header(client, "Accept", "application/json");
    if (request_body) {
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, request_body, static_cast<int>(strlen(request_body)));
    }

    esp_err_t result = esp_http_client_perform(client);
    int status       = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (status_code) *status_code = status;
    if (result != ESP_OK || response.overflow || status < 200 || status >= 300) {
        ESP_LOGW(TAG, "HTTP request failed: status=%d err=%s", status, esp_err_to_name(result));
        return false;
    }
    if (response_body) *response_body = std::move(response.body);
    return true;
}

bool upload_voice_file(const std::string& capture_id, const std::string& path)
{
    hal_storage_prepare_photo_fs_access();
    hal_storage_lock();
    FILE* file = fopen(path.c_str(), "rb");
    if (!file) {
        hal_storage_unlock();
        return false;
    }
    struct stat info = {};
    bool ok          = fstat(fileno(file), &info) == 0 && info.st_size >= 44 && info.st_size <= 2 * 1024 * 1024;
    std::string url  = gateway_url("/device/v1/voice-captures/" + capture_id);
    esp_http_client_config_t config = {};
    config.url                      = url.c_str();
    config.timeout_ms               = 45000;
    config.disable_auto_redirect    = true;
    config.crt_bundle_attach        = esp_crt_bundle_attach;
    esp_http_client_handle_t client = ok ? esp_http_client_init(&config) : nullptr;
    ok                              = client != nullptr && set_device_authorization(client);
    if (ok) {
        esp_http_client_set_method(client, HTTP_METHOD_PUT);
        esp_http_client_set_header(client, "Content-Type", "audio/wav");
        esp_http_client_set_header(client, "Accept", "application/json");
        ok = esp_http_client_open(client, static_cast<int>(info.st_size)) == ESP_OK;
    }
    uint8_t buffer[4096];
    while (ok) {
        size_t count = fread(buffer, 1, sizeof(buffer), file);
        if (count == 0) {
            ok = !ferror(file);
            break;
        }
        int written = esp_http_client_write(client, reinterpret_cast<const char*>(buffer), static_cast<int>(count));
        if (written != static_cast<int>(count)) ok = false;
    }
    int status = 0;
    if (ok) {
        int64_t headers = esp_http_client_fetch_headers(client);
        status          = esp_http_client_get_status_code(client);
        ok              = headers >= 0 && status >= 200 && status < 300;
    }
    fclose(file);
    if (client) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    if (ok) {
        if (mkdir(VOICE_ARCHIVE_DIR, 0750) != 0 && errno != EEXIST) {
            ok = false;
        } else {
            std::string archived = std::string(VOICE_ARCHIVE_DIR) + "/" + capture_id + ".wav";
            ok                   = rename(path.c_str(), archived.c_str()) == 0;
        }
    }
    hal_storage_unlock();
    ESP_LOGI(TAG, "voice upload %s: id=%s status=%d", ok ? "complete" : "deferred", capture_id.c_str(), status);
    return ok;
}

void upload_pending_voice_captures()
{
    hal_storage_prepare_photo_fs_access();
    hal_storage_lock();
    DIR* directory = opendir(VOICE_OUTBOX_DIR);
    std::vector<std::string> pending;
    if (directory) {
        struct dirent* entry = nullptr;
        while ((entry = readdir(directory)) != nullptr && pending.size() < 4) {
            std::string name = entry->d_name;
            if (name.size() <= 4 || name.substr(name.size() - 4) != ".wav") continue;
            std::string id = name.substr(0, name.size() - 4);
            if (id.empty() || id.size() > 96 || !std::all_of(id.begin(), id.end(), [](char ch) {
                    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
                           ch == '.' || ch == '_' || ch == '-';
                })) {
                continue;
            }
            pending.push_back(id);
        }
        closedir(directory);
    }
    hal_storage_unlock();
    for (const std::string& id : pending) {
        if (!upload_voice_file(id, std::string(VOICE_OUTBOX_DIR) + "/" + id + ".wav")) break;
    }
}

bool digest_file(const std::string& path, uint32_t expected_size, std::string* digest_hex)
{
    hal_storage_prepare_photo_fs_access();
    hal_storage_lock();
    FILE* file = fopen(path.c_str(), "rb");
    if (!file) {
        hal_storage_unlock();
        return false;
    }

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    uint8_t buffer[4096];
    size_t total = 0;
    while (true) {
        size_t count = fread(buffer, 1, sizeof(buffer), file);
        if (count > 0) {
            total += count;
            mbedtls_sha256_update(&sha, buffer, count);
        }
        if (count < sizeof(buffer)) break;
    }
    fclose(file);
    hal_storage_unlock();
    unsigned char digest[32];
    mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);
    if (total != expected_size) return false;

    char hex[65];
    for (size_t index = 0; index < sizeof(digest); ++index) {
        snprintf(hex + index * 2, 3, "%02x", digest[index]);
    }
    hex[64] = 0;
    if (digest_hex) *digest_hex = hex;
    return true;
}

bool asset_is_valid(const Job& job)
{
    std::string digest;
    return digest_file(asset_local_path(job.asset_sha256), job.asset_size, &digest) &&
           digest == lower_ascii(job.asset_sha256);
}

Job pulse_page_asset(const PulsePage& page)
{
    Job asset;
    asset.asset_path   = page.asset_path;
    asset.asset_sha256 = page.asset_sha256;
    asset.asset_size   = page.asset_size;
    return asset;
}

bool download_asset(const Job& job)
{
    if (asset_is_valid(job)) return true;

    MemoryResponse response;
    response.limit = MAX_ASSET_BYTES;
    response.body.reserve(job.asset_size);

    std::string url = gateway_url(job.asset_path);

    esp_http_client_config_t config = {};
    config.url                      = url.c_str();
    config.timeout_ms               = 30000;
    config.event_handler            = memory_event_handler;
    config.user_data                = &response;
    config.disable_auto_redirect    = true;
    config.crt_bundle_attach        = esp_crt_bundle_attach;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    bool ok                         = client != nullptr && set_device_authorization(client);
    int status                      = 0;
    esp_err_t result                = ESP_FAIL;
    if (ok) {
        esp_http_client_set_header(client, "Accept", "image/png");
        result = esp_http_client_perform(client);
        status = esp_http_client_get_status_code(client);
    }
    if (client) {
        esp_http_client_cleanup(client);
    }

    unsigned char digest[32];
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    if (!response.body.empty()) mbedtls_sha256_update(&sha, response.body.data(), response.body.size());
    mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);

    char digest_hex[65];
    for (size_t index = 0; index < sizeof(digest); ++index) {
        snprintf(digest_hex + index * 2, 3, "%02x", digest[index]);
    }
    digest_hex[64] = 0;

    ok = ok && result == ESP_OK && status == 200 && !response.overflow && response.body.size() == job.asset_size &&
         lower_ascii(job.asset_sha256) == digest_hex;
    if (!ok || !ensure_cache_dir()) {
        ESP_LOGW(TAG, "asset download failed: status=%d bytes=%u", status, static_cast<unsigned>(response.body.size()));
        return false;
    }

    std::string final_path = asset_local_path(job.asset_sha256);
    std::string temp_path  = final_path + ".tmp";
    hal_storage_prepare_photo_fs_access();
    hal_storage_lock();
    FILE* file     = fopen(temp_path.c_str(), "wb");
    bool persisted = file != nullptr;
    if (file) {
        persisted = fwrite(response.body.data(), 1, response.body.size(), file) == response.body.size() &&
                    fflush(file) == 0 && fsync(fileno(file)) == 0;
        fclose(file);
    }
    if (persisted) {
        remove(final_path.c_str());
        persisted = rename(temp_path.c_str(), final_path.c_str()) == 0;
    }
    if (!persisted) remove(temp_path.c_str());
    hal_storage_unlock();

    if (!persisted) ESP_LOGW(TAG, "asset persistence failed: %s", final_path.c_str());
    return persisted;
}

bool read_file(const char* path, std::vector<uint8_t>* output, size_t limit)
{
    if (!output) return false;
    hal_storage_prepare_photo_fs_access();
    hal_storage_lock();
    FILE* file = fopen(path, "rb");
    if (!file) {
        hal_storage_unlock();
        return false;
    }
    output->clear();
    uint8_t buffer[2048];
    bool ok = true;
    while (true) {
        size_t count = fread(buffer, 1, sizeof(buffer), file);
        if (output->size() + count > limit) {
            ok = false;
            break;
        }
        output->insert(output->end(), buffer, buffer + count);
        if (count < sizeof(buffer)) break;
    }
    fclose(file);
    hal_storage_unlock();
    return ok;
}

bool write_manifest(const Manifest& manifest)
{
    if (!ensure_cache_dir()) return false;
    cJSON* root = cJSON_CreateObject();
    if (!root) return false;
    cJSON_AddNumberToObject(root, "protocol_version", manifest.protocol_version);
    cJSON_AddNumberToObject(root, "server_time", static_cast<double>(manifest.server_time));
    cJSON_AddNumberToObject(root, "next_sync_at", static_cast<double>(manifest.next_sync_at));
    cJSON_AddNumberToObject(root, "manifest_version", static_cast<double>(manifest.version));
    cJSON* jobs = cJSON_AddArrayToObject(root, "jobs");
    for (const Job& job : manifest.jobs) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "id", job.id.c_str());
        cJSON_AddStringToObject(item, "asset_path", job.asset_path.c_str());
        cJSON_AddStringToObject(item, "asset_sha256", job.asset_sha256.c_str());
        cJSON_AddNumberToObject(item, "asset_size", job.asset_size);
        cJSON_AddNumberToObject(item, "display_at", static_cast<double>(job.display_at));
        cJSON_AddNumberToObject(item, "expires_at", static_cast<double>(job.expires_at));
        cJSON_AddBoolToObject(item, "displayed", job.displayed);
        cJSON_AddItemToArray(jobs, item);
    }
    if (!manifest.pulse_pages.empty()) {
        cJSON* pages = cJSON_AddArrayToObject(root, "pulse_pages");
        for (const PulsePage& page : manifest.pulse_pages) {
            cJSON* item = cJSON_CreateObject();
            cJSON_AddNumberToObject(item, "index", page.index);
            cJSON_AddStringToObject(item, "name", page.name.c_str());
            cJSON_AddStringToObject(item, "asset_path", page.asset_path.c_str());
            cJSON_AddStringToObject(item, "asset_sha256", page.asset_sha256.c_str());
            cJSON_AddNumberToObject(item, "asset_size", page.asset_size);
            cJSON_AddItemToArray(pages, item);
        }
    }
    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return false;

    hal_storage_prepare_photo_fs_access();
    hal_storage_lock();
    FILE* file = fopen(MANIFEST_TEMP_PATH, "wb");
    bool ok    = file != nullptr;
    if (file) {
        size_t length = strlen(json);
        ok            = fwrite(json, 1, length, file) == length && fflush(file) == 0 && fsync(fileno(file)) == 0;
        fclose(file);
    }

    bool active_moved = false;
    if (ok && access(MANIFEST_PATH, F_OK) == 0) {
        if (remove(MANIFEST_BACKUP_PATH) != 0 && errno != ENOENT) {
            ok = false;
        } else {
            active_moved = rename(MANIFEST_PATH, MANIFEST_BACKUP_PATH) == 0;
            ok           = active_moved;
        }
    }
    if (ok) {
        ok = rename(MANIFEST_TEMP_PATH, MANIFEST_PATH) == 0;
        if (!ok && active_moved && rename(MANIFEST_BACKUP_PATH, MANIFEST_PATH) != 0) {
            ESP_LOGE(TAG, "manifest rollback failed: errno=%d", errno);
        }
    }
    if (!ok) remove(MANIFEST_TEMP_PATH);
    hal_storage_unlock();
    cJSON_free(json);
    return ok;
}

void cleanup_unreferenced_assets(const Manifest& manifest)
{
    std::vector<std::string> keep;
    keep.reserve(manifest.jobs.size() + manifest.pulse_pages.size());
    for (const Job& job : manifest.jobs) keep.push_back(lower_ascii(job.asset_sha256) + ".png");
    for (const PulsePage& page : manifest.pulse_pages) keep.push_back(lower_ascii(page.asset_sha256) + ".png");

    hal_storage_prepare_photo_fs_access();
    hal_storage_lock();
    DIR* directory = opendir(CACHE_DIR);
    if (!directory) {
        hal_storage_unlock();
        return;
    }
    struct dirent* entry = nullptr;
    while ((entry = readdir(directory)) != nullptr) {
        std::string name = entry->d_name;
        if (name.size() == 72 && name.compare(64, 8, ".png.tmp") == 0 && is_hex_sha256(name.substr(0, 64))) {
            std::string path = std::string(CACHE_DIR) + "/" + name;
            remove(path.c_str());
            continue;
        }
        if (name.size() != 68 || name.compare(64, 4, ".png") != 0 || !is_hex_sha256(name.substr(0, 64))) continue;
        if (std::find(keep.begin(), keep.end(), lower_ascii(name)) != keep.end()) continue;
        std::string path = std::string(CACHE_DIR) + "/" + name;
        remove(path.c_str());
    }
    closedir(directory);
    hal_storage_unlock();
}

bool json_int64(cJSON* object, const char* name, int64_t* output, bool required = true)
{
    cJSON* value = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!value) return !required;
    if (!cJSON_IsNumber(value)) return false;
    *output = static_cast<int64_t>(value->valuedouble);
    return true;
}

bool parse_manifest(const uint8_t* data, size_t length, Manifest* output, const Manifest* previous)
{
    if (!data || length == 0 || !output) return false;
    cJSON* root = cJSON_ParseWithLength(reinterpret_cast<const char*>(data), length);
    if (!root) return false;

    Manifest parsed;
    cJSON* protocol = cJSON_GetObjectItemCaseSensitive(root, "protocol_version");
    cJSON* version  = cJSON_GetObjectItemCaseSensitive(root, "manifest_version");
    bool ok         = cJSON_IsNumber(protocol) && protocol->valueint == 1 && cJSON_IsNumber(version) &&
              json_int64(root, "server_time", &parsed.server_time) &&
              json_int64(root, "next_sync_at", &parsed.next_sync_at);
    parsed.protocol_version = 1;
    parsed.version          = cJSON_IsNumber(version) ? static_cast<uint64_t>(version->valuedouble) : 0;
    if (parsed.server_time < MIN_VALID_EPOCH || parsed.next_sync_at <= parsed.server_time) ok = false;

    cJSON* jobs = cJSON_GetObjectItemCaseSensitive(root, "jobs");
    if (!cJSON_IsArray(jobs) || cJSON_GetArraySize(jobs) > static_cast<int>(MAX_JOBS)) ok = false;

    size_t total_asset_bytes = 0;
    if (ok) {
        cJSON* item = nullptr;
        cJSON_ArrayForEach(item, jobs)
        {
            cJSON* id   = cJSON_GetObjectItemCaseSensitive(item, "id");
            cJSON* path = cJSON_GetObjectItemCaseSensitive(item, "asset_path");
            cJSON* sha  = cJSON_GetObjectItemCaseSensitive(item, "asset_sha256");
            cJSON* size = cJSON_GetObjectItemCaseSensitive(item, "asset_size");
            Job job;
            if (!cJSON_IsString(id) || !id->valuestring || strlen(id->valuestring) == 0 ||
                strlen(id->valuestring) >= 64 || !cJSON_IsString(path) || !path->valuestring ||
                strncmp(path->valuestring, "/device/v1/assets/", 18) != 0 || strstr(path->valuestring, "..") ||
                strlen(path->valuestring) >= 192 || !cJSON_IsString(sha) || !sha->valuestring ||
                !is_hex_sha256(sha->valuestring) || !cJSON_IsNumber(size) || size->valuedouble <= 0 ||
                size->valuedouble > MAX_ASSET_BYTES || !json_int64(item, "display_at", &job.display_at) ||
                !json_int64(item, "expires_at", &job.expires_at, false) || job.display_at <= 0 ||
                (job.expires_at > 0 && job.expires_at <= job.display_at)) {
                ok = false;
                break;
            }
            job.id           = id->valuestring;
            job.asset_path   = path->valuestring;
            job.asset_sha256 = lower_ascii(sha->valuestring);
            job.asset_size   = static_cast<uint32_t>(size->valuedouble);
            total_asset_bytes += job.asset_size;
            if (total_asset_bytes > MAX_CACHE_MANIFEST_BYTES) {
                ok = false;
                break;
            }
            if (previous) {
                auto old = std::find_if(previous->jobs.begin(), previous->jobs.end(),
                                        [&](const Job& candidate) { return candidate.id == job.id; });
                if (old != previous->jobs.end()) job.displayed = old->displayed;
            } else {
                cJSON* displayed = cJSON_GetObjectItemCaseSensitive(item, "displayed");
                job.displayed    = cJSON_IsTrue(displayed);
            }
            parsed.jobs.push_back(std::move(job));
        }
    }

    cJSON* pages = cJSON_GetObjectItemCaseSensitive(root, "pulse_pages");
    if (ok && pages) {
        static constexpr const char* EXPECTED_NAMES[] = {"01-now", "02-work", "03-ideas"};
        if (!cJSON_IsArray(pages) || cJSON_GetArraySize(pages) != 3) {
            ok = false;
        } else {
            cJSON* item        = nullptr;
            int expected_index = 0;
            cJSON_ArrayForEach(item, pages)
            {
                cJSON* index = cJSON_GetObjectItemCaseSensitive(item, "index");
                cJSON* name  = cJSON_GetObjectItemCaseSensitive(item, "name");
                cJSON* path  = cJSON_GetObjectItemCaseSensitive(item, "asset_path");
                cJSON* sha   = cJSON_GetObjectItemCaseSensitive(item, "asset_sha256");
                cJSON* size  = cJSON_GetObjectItemCaseSensitive(item, "asset_size");
                if (!cJSON_IsNumber(index) || index->valueint != expected_index || !cJSON_IsString(name) ||
                    !name->valuestring || strcmp(name->valuestring, EXPECTED_NAMES[expected_index]) != 0 ||
                    !cJSON_IsString(path) || !path->valuestring ||
                    strncmp(path->valuestring, "/device/v1/assets/", 18) != 0 || strstr(path->valuestring, "..") ||
                    strlen(path->valuestring) >= 192 || !cJSON_IsString(sha) || !sha->valuestring ||
                    !is_hex_sha256(sha->valuestring) || !cJSON_IsNumber(size) || size->valuedouble <= 0 ||
                    size->valuedouble > MAX_ASSET_BYTES) {
                    ok = false;
                    break;
                }
                PulsePage page;
                page.index        = static_cast<uint8_t>(expected_index);
                page.name         = name->valuestring;
                page.asset_path   = path->valuestring;
                page.asset_sha256 = lower_ascii(sha->valuestring);
                page.asset_size   = static_cast<uint32_t>(size->valuedouble);
                total_asset_bytes += page.asset_size;
                if (total_asset_bytes > MAX_CACHE_MANIFEST_BYTES) {
                    ok = false;
                    break;
                }
                parsed.pulse_pages.push_back(std::move(page));
                ++expected_index;
            }
        }
    }

    cJSON_Delete(root);
    if (!ok) return false;
    std::sort(parsed.jobs.begin(), parsed.jobs.end(),
              [](const Job& left, const Job& right) { return left.display_at < right.display_at; });
    *output = std::move(parsed);
    return true;
}

bool load_manifest_file(const char* path, Manifest* parsed)
{
    std::vector<uint8_t> body;
    return read_file(path, &body, MAX_MANIFEST_BYTES) && !body.empty() &&
           parse_manifest(body.data(), body.size(), parsed, nullptr);
}

void restore_manifest_backup()
{
    hal_storage_prepare_photo_fs_access();
    hal_storage_lock();
    remove(MANIFEST_PATH);
    if (rename(MANIFEST_BACKUP_PATH, MANIFEST_PATH) != 0) {
        ESP_LOGW(TAG, "failed to restore manifest backup: errno=%d", errno);
    }
    hal_storage_unlock();
}

bool load_manifest()
{
    Manifest parsed;
    bool ok = load_manifest_file(MANIFEST_PATH, &parsed);
    if (!ok && load_manifest_file(MANIFEST_BACKUP_PATH, &parsed)) {
        ESP_LOGW(TAG, "using manifest backup after active manifest failure");
        restore_manifest_backup();
        ok = true;
    }
    g_manifest_loaded = true;
    g_manifest        = ok ? std::move(parsed) : Manifest{};
    return ok;
}

void ensure_manifest_loaded()
{
    if (!g_manifest_loaded) load_manifest();
}

bool save_pending_ack(const char* job_id, const char* state, const char* message)
{
    nvs_handle_t handle;
    if (nvs_open("papercolor", NVS_READWRITE, &handle) != ESP_OK) return false;
    bool ok = nvs_set_str(handle, "gw_ack_id", job_id ? job_id : "") == ESP_OK &&
              nvs_set_str(handle, "gw_ack_state", state ? state : "") == ESP_OK &&
              nvs_set_str(handle, "gw_ack_msg", message ? message : "") == ESP_OK && nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    return ok;
}

bool read_nvs_string(nvs_handle_t handle, const char* key, char* value, size_t size)
{
    size_t required = size;
    return nvs_get_str(handle, key, value, &required) == ESP_OK && value[0];
}

bool send_pending_ack()
{
    nvs_handle_t handle;
    if (nvs_open("papercolor", NVS_READWRITE, &handle) != ESP_OK) return false;
    char job_id[64]  = {};
    char state[16]   = {};
    char message[96] = {};
    bool has_ack     = read_nvs_string(handle, "gw_ack_id", job_id, sizeof(job_id)) &&
                   read_nvs_string(handle, "gw_ack_state", state, sizeof(state));
    read_nvs_string(handle, "gw_ack_msg", message, sizeof(message));
    nvs_close(handle);
    if (!has_ack) return true;

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "device_id", device_id().c_str());
    cJSON_AddStringToObject(root, "job_id", job_id);
    cJSON_AddStringToObject(root, "state", state);
    cJSON_AddStringToObject(root, "message", message);
    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return false;
    int status = 0;
    bool ok    = http_json_request(gateway_url("/device/v1/ack"), HTTP_METHOD_POST, json, nullptr, &status);
    cJSON_free(json);
    if (ok && nvs_open("papercolor", NVS_READWRITE, &handle) == ESP_OK) {
        nvs_erase_key(handle, "gw_ack_id");
        nvs_erase_key(handle, "gw_ack_state");
        nvs_erase_key(handle, "gw_ack_msg");
        nvs_commit(handle);
        nvs_close(handle);
    }
    return ok;
}

bool send_device_telemetry()
{
    int64_t observed_at = hal.rtcEpochUtc();
    if (observed_at < MIN_VALID_EPOCH) return false;
    time_t epoch  = static_cast<time_t>(observed_at);
    struct tm utc = {};
    if (!gmtime_r(&epoch, &utc)) return false;
    char timestamp[32] = {};
    if (strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &utc) == 0) return false;

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "observed_at", timestamp);
    float temperature = 0.0f;
    float humidity    = 0.0f;
    if (hal.sht40CachedRead(&temperature, &humidity)) {
        cJSON_AddNumberToObject(root, "temperature_c", temperature);
        cJSON_AddNumberToObject(root, "humidity_pct", humidity);
    }
    cJSON_AddStringToObject(root, "sd_state", hal.isSDCardInserted() ? "ready" : "missing");
    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return false;
    int status = 0;
    bool ok    = http_json_request(gateway_url("/device/v1/telemetry"), HTTP_METHOD_POST, json, nullptr, &status);
    cJSON_free(json);
    return ok;
}

bool display_pulse_page(PhotoSlideshow& slideshow, size_t index)
{
    ensure_manifest_loaded();
    if (g_manifest.pulse_pages.size() != 3 || index >= g_manifest.pulse_pages.size()) return false;
    const PulsePage& page = g_manifest.pulse_pages[index];
    Job asset             = pulse_page_asset(page);
    if (!asset_is_valid(asset) || !slideshow.displayPhotoByPath(asset_local_path(page.asset_sha256).c_str())) {
        hal.statusEventSend(OPERATION_EVENT_FAILED);
        return false;
    }
    g_pulse_page_index = index;
    hal.statusEventSend(OPERATION_EVENT_SUCCESS);
    ESP_LOGI(TAG, "Pulse page [%u/3] displayed: %s", static_cast<unsigned>(index + 1), page.name.c_str());
    return true;
}

}  // namespace

bool enabled()
{
    return hal.settings.gateway_enabled && hal.settings.gateway_url[0] && hal.settings.gateway_token[0] &&
           hal.settings.gateway_poll_minutes >= 1;
}

void reset_state()
{
    hal_storage_prepare_photo_fs_access();
    hal_storage_lock();
    DIR* directory = opendir(CACHE_DIR);
    if (directory) {
        struct dirent* entry = nullptr;
        while ((entry = readdir(directory)) != nullptr) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
            std::string path = std::string(CACHE_DIR) + "/" + entry->d_name;
            remove(path.c_str());
        }
        closedir(directory);
        rmdir(CACHE_DIR);
    }
    hal_storage_unlock();

    nvs_handle_t handle;
    if (nvs_open("papercolor", NVS_READWRITE, &handle) == ESP_OK) {
        nvs_erase_key(handle, "gw_ack_id");
        nvs_erase_key(handle, "gw_ack_state");
        nvs_erase_key(handle, "gw_ack_msg");
        nvs_commit(handle);
        nvs_close(handle);
    }

    g_manifest                = Manifest{};
    g_manifest_loaded         = false;
    g_next_sync_ms            = 0;
    g_next_display_attempt_ms = 0;
    g_pulse_page_index        = 0;
}

bool show_previous_pulse(PhotoSlideshow& slideshow)
{
    ensure_manifest_loaded();
    if (g_manifest.pulse_pages.size() != 3) return false;
    return display_pulse_page(slideshow,
                              papercolor::previous_pulse_index(g_pulse_page_index, g_manifest.pulse_pages.size()));
}

bool show_next_pulse(PhotoSlideshow& slideshow)
{
    ensure_manifest_loaded();
    if (g_manifest.pulse_pages.size() != 3) return false;
    return display_pulse_page(slideshow,
                              papercolor::next_pulse_index(g_pulse_page_index, g_manifest.pulse_pages.size()));
}

bool show_current_pulse(PhotoSlideshow& slideshow)
{
    ensure_manifest_loaded();
    if (g_manifest.pulse_pages.size() != 3) return false;
    if (g_pulse_page_index >= g_manifest.pulse_pages.size()) g_pulse_page_index = 0;
    return display_pulse_page(slideshow, g_pulse_page_index);
}

bool display_due_cached(PhotoSlideshow& slideshow)
{
    if (!enabled()) return false;
    ensure_manifest_loaded();
    uint32_t attempt_ms = monotonic_ms();
    if (g_next_display_attempt_ms != 0 && static_cast<int32_t>(attempt_ms - g_next_display_attempt_ms) < 0) {
        return false;
    }

    int64_t now = hal.rtcEpochUtc();
    if (now < MIN_VALID_EPOCH || g_manifest.jobs.empty()) return false;

    std::vector<papercolor::ScheduledJobWindow> windows;
    windows.reserve(g_manifest.jobs.size());
    for (const Job& job : g_manifest.jobs) {
        windows.push_back({job.display_at, job.expires_at, job.displayed});
    }

    int selected = papercolor::select_due_job(windows.data(), windows.size(), now);
    if (selected < 0 && hal.isRtcWakeBoot()) {
        int imminent = papercolor::select_imminent_job(windows.data(), windows.size(), now, DISPLAY_WAKE_WAIT_SECONDS);
        if (imminent >= 0) {
            int64_t target           = windows[static_cast<size_t>(imminent)].display_at;
            uint32_t wait_started_ms = monotonic_ms();
            while (now < target &&
                   monotonic_ms() - wait_started_ms <= static_cast<uint32_t>((DISPLAY_WAKE_WAIT_SECONDS + 2) * 1000)) {
                int64_t remaining_ms = (target - now) * 1000;
                uint32_t delay_ms = static_cast<uint32_t>(std::min<int64_t>(500, std::max<int64_t>(100, remaining_ms)));
                vTaskDelay(pdMS_TO_TICKS(delay_ms));
                now = hal.rtcEpochUtc();
                if (now < MIN_VALID_EPOCH) return false;
            }
            selected = papercolor::select_due_job(windows.data(), windows.size(), now);
        }
    }
    if (selected < 0) return false;

    Job& job = g_manifest.jobs[static_cast<size_t>(selected)];
    bool ok  = asset_is_valid(job) && slideshow.displayPhotoByPath(asset_local_path(job.asset_sha256).c_str());
    if (!ok) {
        defer_display_retry(attempt_ms);
        defer_periodic_retry(attempt_ms);
        ESP_LOGW(TAG, "scheduled job %s display deferred; cached asset is unavailable", job.id.c_str());
        return false;
    }

    job.displayed = true;
    size_t suppressed =
        papercolor::suppress_older_due_jobs(windows.data(), windows.size(), static_cast<size_t>(selected), now);
    for (size_t index = 0; index < windows.size(); ++index) {
        if (windows[index].displayed) g_manifest.jobs[index].displayed = true;
    }
    if (!write_manifest(g_manifest)) {
        ESP_LOGW(TAG, "display state persistence failed; retrying synchronization later");
        defer_periodic_retry(attempt_ms);
    }
    g_next_display_attempt_ms = 0;
    if (!save_pending_ack(job.id.c_str(), "displayed", "")) {
        ESP_LOGW(TAG, "failed to persist displayed acknowledgement");
        defer_periodic_retry(attempt_ms);
    }
    ESP_LOGI(TAG, "scheduled job %s displayed; superseded=%u", job.id.c_str(), static_cast<unsigned>(suppressed));
    return true;
}

bool sync_now(PhotoSlideshow& slideshow)
{
    if (!enabled()) return false;
    ensure_manifest_loaded();
    uint32_t attempt_ms = monotonic_ms();
    send_pending_ack();
    upload_pending_voice_captures();
    if (!send_device_telemetry()) {
        ESP_LOGW(TAG, "Pulse telemetry upload failed; continuing with the last gateway snapshot");
    }

    std::string path = "/device/v1/manifest?device_id=" + device_id();
    std::vector<uint8_t> body;
    int status = 0;
    if (!http_json_request(gateway_url(path), HTTP_METHOD_GET, nullptr, &body, &status)) {
        defer_periodic_retry(attempt_ms);
        return false;
    }

    Manifest incoming;
    if (!parse_manifest(body.data(), body.size(), &incoming, &g_manifest)) {
        ESP_LOGW(TAG, "gateway returned an invalid manifest");
        defer_periodic_retry(attempt_ms);
        return false;
    }
    if (!hal.setRtcEpochUtc(incoming.server_time)) {
        ESP_LOGW(TAG, "failed to refresh the external RTC from gateway time");
        defer_periodic_retry(attempt_ms);
        return false;
    }

    incoming.jobs.erase(
        std::remove_if(incoming.jobs.begin(), incoming.jobs.end(),
                       [&](const Job& job) { return job.expires_at > 0 && job.expires_at <= incoming.server_time; }),
        incoming.jobs.end());
    for (const Job& job : incoming.jobs) {
        if (download_asset(job)) continue;
        defer_periodic_retry(attempt_ms);
        return false;
    }
    for (const PulsePage& page : incoming.pulse_pages) {
        if (download_asset(pulse_page_asset(page))) continue;
        defer_periodic_retry(attempt_ms);
        return false;
    }
    bool pulse_changed = incoming.pulse_pages.size() != g_manifest.pulse_pages.size();
    if (!pulse_changed) {
        for (size_t index = 0; index < incoming.pulse_pages.size(); ++index) {
            if (incoming.pulse_pages[index].asset_sha256 != g_manifest.pulse_pages[index].asset_sha256) {
                pulse_changed = true;
                break;
            }
        }
    }
    if (!write_manifest(incoming)) {
        ESP_LOGW(TAG, "incoming manifest persistence failed; retaining the previous generation");
        defer_periodic_retry(attempt_ms);
        return false;
    }

    g_manifest        = std::move(incoming);
    g_manifest_loaded = true;
    if (pulse_changed) g_pulse_page_index = 0;
    cleanup_unreferenced_assets(g_manifest);
    defer_periodic_sync(monotonic_ms());
    g_next_display_attempt_ms = 0;
    bool displayed_job        = display_due_cached(slideshow);
    if (!displayed_job && pulse_changed) show_current_pulse(slideshow);
    if (!send_pending_ack()) {
        defer_periodic_retry(monotonic_ms());
        return false;
    }
    return true;
}

bool periodic_sync_due(uint32_t now_ms)
{
    return enabled() && (g_next_sync_ms == 0 || static_cast<int32_t>(now_ms - g_next_sync_ms) >= 0);
}

void defer_periodic_sync(uint32_t now_ms)
{
    int64_t delay_seconds = static_cast<int64_t>(std::max<uint16_t>(1, hal.settings.gateway_poll_minutes)) * 60;
    int64_t rtc_now       = hal.rtcEpochUtc();
    if (rtc_now >= MIN_VALID_EPOCH) {
        int64_t deadline =
            papercolor::select_next_sync_deadline(rtc_now, g_manifest.next_sync_at, hal.settings.gateway_poll_minutes);
        delay_seconds = std::max<int64_t>(1, deadline - rtc_now);
    }
    g_next_sync_ms = now_ms + static_cast<uint32_t>(delay_seconds * 1000);
}

int64_t next_wake_epoch()
{
    ensure_manifest_loaded();
    int64_t now = hal.rtcEpochUtc();
    if (now < MIN_VALID_EPOCH) return 0;
    int64_t next_sync =
        papercolor::select_next_sync_deadline(now, g_manifest.next_sync_at, hal.settings.gateway_poll_minutes);

    std::vector<papercolor::ScheduledJobWindow> windows;
    windows.reserve(g_manifest.jobs.size());
    for (const Job& job : g_manifest.jobs) {
        windows.push_back({job.display_at, job.expires_at, job.displayed});
    }
    int64_t selected = papercolor::select_next_wake(windows.data(), windows.size(), now, next_sync,
                                                    DISPLAY_WAKE_LEAD_SECONDS, RETRY_SECONDS);
    return selected > now ? selected : now + RETRY_SECONDS;
}

bool schedule_next_wake_and_power_off()
{
    if (!enabled()) return false;
    int64_t wake = next_wake_epoch();
    if (wake <= 0) return false;
    hal.clearWakeFlags();
    if (!hal.configureRtcWakePin() || !hal.scheduleWakeEpochUtc(wake)) {
        ESP_LOGW(TAG, "failed to program scheduled gateway wake");
        return false;
    }
    ESP_LOGI(TAG, "powering off until UTC epoch %lld", static_cast<long long>(wake));
    vTaskDelay(pdMS_TO_TICKS(50));
    if (!hal.powerOff()) {
        ESP_LOGW(TAG, "PMIC rejected the power-off command");
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGW(TAG, "power-off command returned without power loss");
    return false;
}

}  // namespace scheduled_push
