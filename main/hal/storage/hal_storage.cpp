#include "hal_storage.h"
#include <errno.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_partition.h"
#include "driver/gpio.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"
#include "sdmmc_cmd.h"
#include "diskio_impl.h"
#include "diskio_sdmmc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "hal_storage";

typedef struct {
    bool driver_installed;
    bool storage_created;
    hal_storage_media_t media;
    tinyusb_msc_storage_handle_t storage_hdl;
    /* Low-level resources */
    wl_handle_t wl_handle;
    sdmmc_card_t *sd_card;
    sdmmc_host_t sd_host;
    bool sd_host_init;
    sdspi_dev_handle_t sd_spi_handle;  // Added: records the sdspi device handle
    bool sd_spi_attached;              // Added: whether it is already attached to the bus
} hal_storage_ctx_t;

bool s_spi_bus_inited = false;

static hal_storage_ctx_t s_ctx = {
    /* .driver_installed = */ false,
    /* .storage_created  = */ false,
    /* .media            = */ APP_STORAGE_MEDIA_SPIFLASH,
    /* .storage_hdl      = */ NULL,
    /* .wl_handle        = */ WL_INVALID_HANDLE,
    /* .sd_card          = */ NULL,
    /* .sd_host          = */ {},
    /* .sd_host_init     = */ false,
    /* .sd_spi_handle    = */ 0,
    /* .sd_spi_attached  = */ false,
};

static SemaphoreHandle_t s_storage_lock = NULL;

static void ensure_storage_lock(void)
{
    if (!s_storage_lock) {
        s_storage_lock = xSemaphoreCreateMutex();
    }
}

void hal_storage_lock(void)
{
    ensure_storage_lock();
    if (s_storage_lock) xSemaphoreTake(s_storage_lock, portMAX_DELAY);
}

void hal_storage_unlock(void)
{
    if (s_storage_lock) xSemaphoreGive(s_storage_lock);
}

void hal_storage_prepare_photo_fs_access(void)
{
#if PHOTO_OPS_FORCE_APP_MOUNT
    storage_mount_to_app();
#endif
}

#define SD_SPI_HOST SPI2_HOST
#define SD_PIN_MOSI GPIO_NUM_13
#define SD_PIN_MISO GPIO_NUM_14
#define SD_PIN_SCLK GPIO_NUM_15
#define SD_PIN_CS   GPIO_NUM_47

#define TUSB_DESC_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN)
#define BASE_PATH           "/data"

static char const *string_desc_arr[] = {
    (const char[]){0x09, 0x04}, "M5Stack", "PaperColor", "", "M5Stack PaperColor",
};

enum { ITF_NUM_MSC = 0, ITF_NUM_TOTAL };
enum {
    EDPT_CTRL_OUT = 0x00,
    EDPT_CTRL_IN  = 0x80,
    EDPT_MSC_OUT  = 0x01,
    EDPT_MSC_IN   = 0x81,
};

static tusb_desc_device_t descriptor_config = {
    .bLength            = sizeof(descriptor_config),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0x303A,
    .idProduct          = 0x4002,
    .bcdDevice          = 0x100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};

static uint8_t const msc_fs_configuration_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, TUSB_DESC_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, 0, EDPT_MSC_OUT, EDPT_MSC_IN, 64),
};

static void storage_mount_changed_cb(tinyusb_msc_storage_handle_t handle, tinyusb_msc_event_t *event, void *arg)
{
    switch (event->id) {
        case TINYUSB_MSC_EVENT_MOUNT_START:
            ESP_LOGI(TAG, "Mount start, target=%s",
                     (event->mount_point == TINYUSB_MSC_STORAGE_MOUNT_APP) ? "APP" : "USB");
            break;
        case TINYUSB_MSC_EVENT_MOUNT_COMPLETE:
            ESP_LOGI(TAG, "Mount complete, mounted to %s",
                     (event->mount_point == TINYUSB_MSC_STORAGE_MOUNT_APP) ? "APP" : "USB");
            break;
        case TINYUSB_MSC_EVENT_MOUNT_FAILED:
        case TINYUSB_MSC_EVENT_FORMAT_REQUIRED:
            ESP_LOGE(TAG, "Storage mount failed or format required");
            break;
        default:
            break;
    }
}

static void usb_event_cb(tinyusb_event_t *event, void *arg)
{
    switch (event->id) {
        case TINYUSB_EVENT_ATTACHED:
            ESP_LOGI(TAG, "USB attached");
            break;

        case TINYUSB_EVENT_DETACHED:
            break;

        case TINYUSB_EVENT_SUSPENDED:
            ESP_LOGI(TAG, "USB suspended");
            storage_mount_to_app();
            break;

        case TINYUSB_EVENT_RESUMED:
            break;
    }
}

static esp_err_t install_tinyusb_device_driver(void)
{
    if (s_ctx.driver_installed) {
        return ESP_OK;
    }

    tinyusb_config_t tusb_cfg             = TINYUSB_DEFAULT_CONFIG();
    tusb_cfg.descriptor.device            = &descriptor_config;
    tusb_cfg.descriptor.full_speed_config = msc_fs_configuration_desc;
    tusb_cfg.descriptor.string            = string_desc_arr;
    tusb_cfg.descriptor.string_count      = sizeof(string_desc_arr) / sizeof(string_desc_arr[0]);
    tusb_cfg.event_cb                     = usb_event_cb;

    esp_err_t ret = tinyusb_driver_install(&tusb_cfg);
    if (ret == ESP_OK) {
        s_ctx.driver_installed = true;
    }
    return ret;
}

static esp_err_t uninstall_tinyusb_device_driver(void)
{
    if (!s_ctx.driver_installed) {
        return ESP_OK;
    }

    esp_err_t ret = tinyusb_driver_uninstall();
    if (ret == ESP_OK) {
        s_ctx.driver_installed = false;
    }
    return ret;
}

static esp_err_t _mount(void)
{
    esp_err_t ret = tinyusb_msc_set_storage_mount_point(s_ctx.storage_hdl, TINYUSB_MSC_STORAGE_MOUNT_APP);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to switch storage mount point to APP: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "ls %s:", BASE_PATH);
    DIR *directory_handle = opendir(BASE_PATH);

    if (!directory_handle) {
        if (errno == ENOENT) {
            ESP_LOGE(TAG, "Directory doesn't exist %s", BASE_PATH);
            ret = ESP_ERR_NOT_FOUND;
        } else {
            ESP_LOGE(TAG, "Unable to read directory %s", BASE_PATH);
            ret = ESP_FAIL;
        }
        tinyusb_msc_set_storage_mount_point(s_ctx.storage_hdl, TINYUSB_MSC_STORAGE_MOUNT_USB);
        return ret;
    }
    struct dirent *entry;
    while ((entry = readdir(directory_handle)) != NULL) {
        printf("  %s\n", entry->d_name);
    }
    closedir(directory_handle);
    return ESP_OK;
}

static esp_err_t storage_init_spiflash(wl_handle_t *wl_handle)
{
    const esp_partition_t *data_partition =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, NULL);
    if (!data_partition) {
        ESP_LOGE(TAG, "Failed to find FATFS partition");
        return ESP_ERR_NOT_FOUND;
    }
    return wl_mount(data_partition, wl_handle);
}

static void storage_deinit_spiflash(void)
{
    if (s_ctx.wl_handle != WL_INVALID_HANDLE) {
        wl_unmount(s_ctx.wl_handle);
        s_ctx.wl_handle = WL_INVALID_HANDLE;
    }
}

static esp_err_t bus_spi_init_once(void)
{
    if (s_spi_bus_inited) return ESP_OK;

    spi_bus_config_t bus_cfg = {
        .mosi_io_num     = SD_PIN_MOSI,
        .miso_io_num     = SD_PIN_MISO,
        .sclk_io_num     = SD_PIN_SCLK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 8192,
    };
    esp_err_t ret = spi_bus_initialize(SD_SPI_HOST, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret == ESP_OK) {
        s_spi_bus_inited = true;
    } else if (ret == ESP_ERR_INVALID_STATE) {
        s_spi_bus_inited = true;
        ret              = ESP_OK;
    }
    return ret;
}

static esp_err_t storage_init_sdmmc(sdmmc_card_t **card, sdmmc_host_t *host_out, bool *host_init_out,
                                    sdspi_dev_handle_t *spi_handle_out, bool *spi_attached_out)
{
    esp_err_t ret                 = ESP_OK;
    bool host_init                = false;
    bool spi_attached             = false;
    sdmmc_card_t *sd_card         = NULL;
    sdspi_dev_handle_t spi_handle = -1;
    int retry                     = 3;

    int supported_freqs[] = {20000, 10000, 4000};
    bool init_ok          = false;

    sdmmc_host_t host                 = SDSPI_HOST_DEFAULT();
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();

    host.slot         = SD_SPI_HOST;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    slot_config.host_id = SD_SPI_HOST;
    slot_config.gpio_cs = SD_PIN_CS;

    ESP_GOTO_ON_ERROR(bus_spi_init_once(), clean, TAG, "spi bus init fail");

    sd_card = (sdmmc_card_t *)malloc(sizeof(sdmmc_card_t));
    ESP_GOTO_ON_FALSE(sd_card, ESP_ERR_NO_MEM, clean, TAG, "alloc sdmmc_card_t fail");

    ESP_GOTO_ON_ERROR(sdspi_host_init(), clean, TAG, "sdspi host init fail");
    host_init = true;

    ESP_GOTO_ON_ERROR(sdspi_host_init_device(&slot_config, &spi_handle), clean, TAG, "sdspi attach fail");
    spi_attached = true;
    host.slot    = spi_handle;

    for (int frequency_index = 0; frequency_index < 3 && !init_ok; frequency_index++) {
        host.max_freq_khz = supported_freqs[frequency_index];
        for (int retry_index = 0; retry_index < 2; retry_index++) {
            if (sdmmc_card_init(&host, sd_card) == ESP_OK) {
                ESP_LOGI(TAG, "SD card init OK @ %d KHz", supported_freqs[frequency_index]);
                init_ok = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
    ESP_GOTO_ON_FALSE(init_ok, ESP_FAIL, clean, TAG, "SD card init failed at all freqs");

    sdmmc_card_print_info(stdout, sd_card);

    *card             = sd_card;
    *host_out         = host;
    *host_init_out    = true;
    *spi_handle_out   = spi_handle;
    *spi_attached_out = true;
    return ESP_OK;

clean:
    if (spi_attached) {
        sdspi_host_remove_device(spi_handle);
    }
    if (host_init) {
        sdspi_host_deinit();
    }
    if (sd_card) {
        free(sd_card);
    }
    return ret;
}

static void storage_deinit_sdmmc(void)
{
    if (s_ctx.sd_spi_attached) {
        sdspi_host_remove_device(s_ctx.sd_spi_handle);
        s_ctx.sd_spi_attached = false;
    }

    if (s_ctx.sd_host_init) {
        sdspi_host_deinit();
        s_ctx.sd_host_init = false;
    }

    if (s_ctx.sd_card) {
        free(s_ctx.sd_card);
        s_ctx.sd_card = NULL;
    }
}

static esp_err_t _bringup(hal_storage_media_t media)
{
    esp_err_t ret = ESP_OK;

    tinyusb_msc_storage_config_t storage_cfg = {};
    storage_cfg.mount_point                  = TINYUSB_MSC_STORAGE_MOUNT_USB;
    storage_cfg.fat_fs.base_path             = (const char *)BASE_PATH;
    storage_cfg.fat_fs.config.max_files      = 5;
    storage_cfg.fat_fs.format_flags          = 0;
    storage_cfg.fat_fs.do_not_format         = true;

    if (media == APP_STORAGE_MEDIA_SPIFLASH) {
        ret = storage_init_spiflash(&s_ctx.wl_handle);
        ESP_RETURN_ON_ERROR(ret, TAG, "spiflash init fail");
        storage_cfg.medium.wl_handle = s_ctx.wl_handle;

        ret = tinyusb_msc_new_storage_spiflash(&storage_cfg, &s_ctx.storage_hdl);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "new_storage_spiflash fail: %s", esp_err_to_name(ret));
            storage_deinit_spiflash();
            return ret;
        }
    } else {
        ret = storage_init_sdmmc(&s_ctx.sd_card, &s_ctx.sd_host, &s_ctx.sd_host_init, &s_ctx.sd_spi_handle,
                                 &s_ctx.sd_spi_attached);
        ESP_RETURN_ON_ERROR(ret, TAG, "sdmmc init fail");

        storage_cfg.medium.card = s_ctx.sd_card;
        ret                     = tinyusb_msc_new_storage_sdmmc(&storage_cfg, &s_ctx.storage_hdl);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "new_storage_sdmmc fail: %s", esp_err_to_name(ret));
            storage_deinit_sdmmc();
            return ret;
        }
    }

    s_ctx.media           = media;
    s_ctx.storage_created = true;

    ret = tinyusb_msc_set_storage_callback(storage_mount_changed_cb, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "set_storage_callback fail: %s", esp_err_to_name(ret));
        goto err_after_storage;
    }
    ret = _mount();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Storage mount to APP failed: %s", esp_err_to_name(ret));
        goto err_after_storage;
    }

    ret = install_tinyusb_device_driver();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_driver_install fail: %s", esp_err_to_name(ret));
        goto err_after_storage;
    }

    return ESP_OK;

err_after_storage:
    uninstall_tinyusb_device_driver();
    if (s_ctx.storage_hdl) {
        tinyusb_msc_mount_point_t cur;
        if (tinyusb_msc_get_storage_mount_point(s_ctx.storage_hdl, &cur) == ESP_OK &&
            cur == TINYUSB_MSC_STORAGE_MOUNT_APP) {
            esp_err_t unmount_ret =
                tinyusb_msc_set_storage_mount_point(s_ctx.storage_hdl, TINYUSB_MSC_STORAGE_MOUNT_USB);
            if (unmount_ret != ESP_OK) {
                ESP_LOGW(TAG, "Failed to switch storage mount point to USB during cleanup: %s",
                         esp_err_to_name(unmount_ret));
            }
        }
        esp_err_t delete_ret = tinyusb_msc_delete_storage(s_ctx.storage_hdl);
        if (delete_ret != ESP_OK) {
            ESP_LOGW(TAG, "delete_storage during cleanup failed: %s", esp_err_to_name(delete_ret));
        }
        s_ctx.storage_hdl = NULL;
    }
    s_ctx.storage_created = false;
    if (media == APP_STORAGE_MEDIA_SPIFLASH) {
        storage_deinit_spiflash();
    } else {
        storage_deinit_sdmmc();
    }
    return ret;
}

esp_err_t hal_storage_init(hal_storage_media_t media)
{
    hal_storage_lock();
    esp_err_t ret;
    if (s_ctx.driver_installed || s_ctx.storage_created) {
        ESP_LOGW(TAG, "Storage already initialized");
        ret = ESP_ERR_INVALID_STATE;
    } else {
        ret = _bringup(media);
    }
    hal_storage_unlock();
    return ret;
}

esp_err_t hal_storage_switch(hal_storage_media_t media)
{
    hal_storage_lock();

    if ((s_ctx.storage_created || s_ctx.driver_installed) && s_ctx.media == media) {
        ESP_LOGW(TAG, "Same media, no need to switch");
        hal_storage_unlock();
        return ESP_OK;
    }

    esp_err_t ret = ESP_OK;

    ret = uninstall_tinyusb_device_driver();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_driver_uninstall fail: %s", esp_err_to_name(ret));
        hal_storage_unlock();
        return ret;
    }

    if (s_ctx.storage_created && s_ctx.storage_hdl) {
        tinyusb_msc_mount_point_t cur;
        if (tinyusb_msc_get_storage_mount_point(s_ctx.storage_hdl, &cur) == ESP_OK &&
            cur == TINYUSB_MSC_STORAGE_MOUNT_APP) {
            ret = tinyusb_msc_set_storage_mount_point(s_ctx.storage_hdl, TINYUSB_MSC_STORAGE_MOUNT_USB);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "switch mount point to USB fail: %s", esp_err_to_name(ret));
                hal_storage_unlock();
                return ret;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        ret = tinyusb_msc_delete_storage(s_ctx.storage_hdl);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "delete_storage fail: %s", esp_err_to_name(ret));
            hal_storage_unlock();
            return ret;
        }
        s_ctx.storage_hdl     = NULL;
        s_ctx.storage_created = false;
    }

    if (s_ctx.media == APP_STORAGE_MEDIA_SPIFLASH) {
        storage_deinit_spiflash();
    } else {
        storage_deinit_sdmmc();
    }

    tinyusb_msc_storage_config_t storage_cfg = {};
    storage_cfg.mount_point                  = TINYUSB_MSC_STORAGE_MOUNT_USB;
    storage_cfg.fat_fs.base_path             = BASE_PATH;
    storage_cfg.fat_fs.config.max_files      = 5;
    storage_cfg.fat_fs.format_flags          = 0;
    storage_cfg.fat_fs.do_not_format         = true;

    if (media == APP_STORAGE_MEDIA_SPIFLASH) {
        ret = storage_init_spiflash(&s_ctx.wl_handle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "spiflash init fail: %s", esp_err_to_name(ret));
            hal_storage_unlock();
            return ret;
        }

        storage_cfg.medium.wl_handle = s_ctx.wl_handle;
        ret                          = tinyusb_msc_new_storage_spiflash(&storage_cfg, &s_ctx.storage_hdl);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "new_storage_spiflash fail: %s", esp_err_to_name(ret));
            storage_deinit_spiflash();
            hal_storage_unlock();
            return ret;
        }
    } else {
        ret = storage_init_sdmmc(&s_ctx.sd_card, &s_ctx.sd_host, &s_ctx.sd_host_init, &s_ctx.sd_spi_handle,
                                 &s_ctx.sd_spi_attached);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "sdmmc init fail: %s", esp_err_to_name(ret));
            hal_storage_unlock();
            return ret;
        }

        storage_cfg.medium.card = s_ctx.sd_card;
        ret                     = tinyusb_msc_new_storage_sdmmc(&storage_cfg, &s_ctx.storage_hdl);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "new_storage_sdmmc fail: %s", esp_err_to_name(ret));
            storage_deinit_sdmmc();
            hal_storage_unlock();
            return ret;
        }
    }

    s_ctx.media           = media;
    s_ctx.storage_created = true;

    ret = tinyusb_msc_set_storage_callback(storage_mount_changed_cb, NULL);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "set_storage_callback fail: %s", esp_err_to_name(ret));
    }

    ret = _mount();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Storage mount to APP failed after switch: %s", esp_err_to_name(ret));
        if (s_ctx.storage_hdl) {
            esp_err_t delete_ret = tinyusb_msc_delete_storage(s_ctx.storage_hdl);
            if (delete_ret != ESP_OK) {
                ESP_LOGW(TAG, "delete_storage after mount failure failed: %s", esp_err_to_name(delete_ret));
            }
            s_ctx.storage_hdl = NULL;
        }
        s_ctx.storage_created = false;
        if (media == APP_STORAGE_MEDIA_SPIFLASH) {
            storage_deinit_spiflash();
        } else {
            storage_deinit_sdmmc();
        }
        hal_storage_unlock();
        return ret;
    }

    ret = install_tinyusb_device_driver();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_driver_install fail after switch: %s", esp_err_to_name(ret));
        if (s_ctx.storage_hdl) {
            tinyusb_msc_mount_point_t cur;
            if (tinyusb_msc_get_storage_mount_point(s_ctx.storage_hdl, &cur) == ESP_OK &&
                cur == TINYUSB_MSC_STORAGE_MOUNT_APP) {
                esp_err_t unmount_ret =
                    tinyusb_msc_set_storage_mount_point(s_ctx.storage_hdl, TINYUSB_MSC_STORAGE_MOUNT_USB);
                if (unmount_ret != ESP_OK) {
                    ESP_LOGW(TAG, "Failed to switch storage mount point to USB during install cleanup: %s",
                             esp_err_to_name(unmount_ret));
                }
            }
            esp_err_t delete_ret = tinyusb_msc_delete_storage(s_ctx.storage_hdl);
            if (delete_ret != ESP_OK) {
                ESP_LOGW(TAG, "delete_storage after install failure failed: %s", esp_err_to_name(delete_ret));
            }
            s_ctx.storage_hdl = NULL;
        }
        s_ctx.storage_created = false;
        if (media == APP_STORAGE_MEDIA_SPIFLASH) {
            storage_deinit_spiflash();
        } else {
            storage_deinit_sdmmc();
        }
        hal_storage_unlock();
        return ret;
    }
    hal_storage_unlock();
    return ESP_OK;
}

hal_storage_media_t hal_storage_get_media(void)
{
    return s_ctx.media;
}

void storage_mount_to_app(void)
{
    hal_storage_lock();
    if (s_ctx.storage_created && s_ctx.storage_hdl) {
        tinyusb_msc_mount_point_t cur;
        if (tinyusb_msc_get_storage_mount_point(s_ctx.storage_hdl, &cur) == ESP_OK) {
            if (cur != TINYUSB_MSC_STORAGE_MOUNT_APP) {
                tinyusb_msc_set_storage_mount_point(s_ctx.storage_hdl, TINYUSB_MSC_STORAGE_MOUNT_APP);
            }
        }
    }
    hal_storage_unlock();
}
