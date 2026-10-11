#pragma once

#include <stdint.h>

// Charge policy shared by the charger setup, the estimate and what the app sees.
//
// The cell is never charged past about 95% of its capacity: the charger
// regulation voltage is lowered to 4.16 V and terminates at 64 mA. That point
// is shown to the user as 100%, so the scale is transparent: every percent the
// board publishes over Bluetooth or HTTP is shown_percent(real).
namespace battery_policy {
constexpr uint8_t kRealFullPercent = 95;
constexpr uint16_t kChargeVoltageMv = 4160;
constexpr uint16_t kTerminationMilliamps = 64;

// LP603048, 900 mAh: normal stays at 1C, gentle at 0.5C.
constexpr uint16_t kNormalChargeMilliamps = 896;
constexpr uint16_t kGentleChargeMilliamps = 448;

// Safe charging holds the shown percent around 90: it stops at 90 and starts
// a very gentle top-up (about 0.2C) only when the shown percent drops below 80.
constexpr uint8_t kSafeStopShownPercent = 90;
constexpr uint8_t kSafeResumeBelowShownPercent = 80;
constexpr uint16_t kSafeTopUpMilliamps = 192;

inline uint8_t shown_percent(uint8_t real)
{
    const uint32_t shown = (static_cast<uint32_t>(real) * 100U + kRealFullPercent / 2U) / kRealFullPercent;
    return static_cast<uint8_t>(shown > 100U ? 100U : shown);
}

inline uint16_t shown_tenths(uint16_t real_tenths)
{
    const uint32_t shown = (static_cast<uint32_t>(real_tenths) * 100U + kRealFullPercent / 2U) / kRealFullPercent;
    return static_cast<uint16_t>(shown > 1000U ? 1000U : shown);
}
}
