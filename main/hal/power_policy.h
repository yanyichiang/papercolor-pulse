#pragma once

#include <cstdint>

namespace papercolor {

constexpr uint16_t kLowBatteryMv      = 3100;
constexpr uint16_t kUsbPowerPresentMv = 4000;

constexpr bool should_shutdown_for_low_battery(bool battery_read_ok, uint16_t battery_mv, bool vin_read_ok,
                                               uint16_t vin_mv)
{
    const bool battery_is_low = battery_read_ok && battery_mv < kLowBatteryMv;
    const bool usb_is_present = vin_read_ok && vin_mv >= kUsbPowerPresentMv;
    return battery_is_low && !usb_is_present;
}

}  // namespace papercolor
