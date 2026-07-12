set(APP_ASSETS_USE_EMBEDDED 1)

idf_component_register(
    SRCS
        "main.cpp"
        "apps/app_manager/app_manager.cpp"
        "apps/app_server/app_server.cpp"
        "apps/scheduled_push/scheduled_push.cpp"
        "apps/voice_capture/voice_capture.cpp"
        "hal/storage/hal_storage.cpp"
        "hal/wifi/hal_wifi.cpp"
        "hal/hal.cpp"
        "apps/local_photo_slideshow/local_photo_slideshow.cpp"
        "hal/utils/image/image_utils.cpp"
        "hal/utils/audio/audio.cpp"
        "hal/utils/dns_server/dns_server.c"
    INCLUDE_DIRS
        "."
        "hal/utils/audio"
        "hal/utils/image"
        "hal/utils/dns_server"
    REQUIRES
        esp_http_server
        esp_http_client
        esp_wifi
        nvs_flash
        fatfs
        wear_levelling
        json
        mbedtls
        esp_event
        esp_netif
        console
        M5Unified
        M5GFX
        tinyusb
        json

)

# Never inject authentication material through compiler definitions. Values in
# compiler command lines, build logs, ELF files, and firmware images are not
# secret storage. Machine-only diagnostics remain fail-closed unless a future
# runtime credential source is explicitly implemented.
target_compile_definitions(${COMPONENT_LIB} PRIVATE APP_ASSETS_USE_EMBEDDED=${APP_ASSETS_USE_EMBEDDED})
target_add_binary_data(${COMPONENT_LIB} "apps/app_server/index.html" TEXT)
target_add_binary_data(${COMPONENT_LIB} "assets/PaperColor.png" BINARY)
target_add_binary_data(${COMPONENT_LIB} "assets/arrowClickToPowerOn.png" BINARY)
