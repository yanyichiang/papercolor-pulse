/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "M5GFX.h"
#include "apps/app_server/app_server.h"
#include <dirent.h>
#include <M5PM1.h>
#include "hal/hal.h"
#include "apps/app_manager/app_manager.h"
#include "assets/boot_sfx.h"

using namespace hal_wifi;
Hal hal;

extern "C" void app_main(void)
{
    hal.init();
    hal.detectWakeSource();
    vTaskDelay(pdMS_TO_TICKS(500));

    hal.settingsInit();

    if (!hal.isRtcWakeBoot() && hal.settings.boot_sound) {
        M5.Speaker.setVolume(200);
        M5.Speaker.playWav(boot_sfx, sizeof(boot_sfx));
        vTaskDelay(pdMS_TO_TICKS(300));
    }

    app_manager_start();
    app_server_init();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
