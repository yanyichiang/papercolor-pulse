#include <cassert>

#include "main/hal/rtc_alarm_policy.h"

int main()
{
    using papercolor::select_calendar_alarm_epoch;

    assert(select_calendar_alarm_epoch(1000, 1205) == 1200);
    assert(select_calendar_alarm_epoch(1194, 1205) == 1200);

    // Keep the device powered when shutdown could race the minute alarm.
    assert(select_calendar_alarm_epoch(1195, 1205) == 0);
    assert(select_calendar_alarm_epoch(1200, 1259) == 0);

    // The RX8130 alarm encodes day/hour/minute, so reject distant epochs.
    assert(select_calendar_alarm_epoch(1000, 1000 + 24 * 60 * 60) == 87360);
    assert(select_calendar_alarm_epoch(1000, 1000 + 24 * 60 * 60 + 1) == 0);

    // UTC day rollover remains a normal minute-floor operation.
    assert(select_calendar_alarm_epoch(86300, 86410) == 86400);
    return 0;
}
