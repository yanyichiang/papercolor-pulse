#pragma once

#include <cstdint>

namespace papercolor {

constexpr int64_t kRtcAlarmArmingMarginSeconds = 5;
constexpr int64_t kRtcAlarmMaxHorizonSeconds   = 24 * 60 * 60;

constexpr int64_t select_calendar_alarm_epoch(int64_t now, int64_t target)
{
    if (now <= 0 || target <= now || target - now > kRtcAlarmMaxHorizonSeconds) return 0;

    const int64_t alarm = target - (target % 60);
    if (alarm <= now + kRtcAlarmArmingMarginSeconds) return 0;
    return alarm;
}

}  // namespace papercolor
