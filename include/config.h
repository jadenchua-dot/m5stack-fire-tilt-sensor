#pragma once

// Wi-Fi name/password and ntfy topic live in secrets.h (git-ignored).
// First time: copy secrets.example.h -> secrets.h and fill it in.
#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Missing include/secrets.h - copy include/secrets.example.h to include/secrets.h and fill it in"
#endif

// If Wi-Fi fails, the device makes its own hotspot instead and the
// dashboard is at http://192.168.4.1
#define AP_SSID        "TiltSensor"
#define AP_PASSWORD    "tiltsensor"      // must be 8+ characters

// Dashboard also reachable at http://<hostname>.local on most networks
#define DEVICE_HOSTNAME "tiltsensor"

// ======================= TUNING ===========================
#define DEFAULT_LIMIT_DEG  10.0f   // tilt (degrees) that triggers the alarm
#define WARN_FRACTION      0.7f    // amber "careful" zone starts at 70% of the limit
#define ALARM_DELAY_MS     500     // must stay over the limit this long before alarming
#define HYSTERESIS_DEG     1.5f    // must come back this far under the limit to clear
#define BEEP_INTERVAL_MS   1000    // beep repeat while tilted
#define SPEAKER_VOLUME     160     // 0-255
#define LED_BRIGHTNESS     40      // side LED bars, 0-255

// ======================= SCREEN AUTO-ROTATE ===============
#define AUTO_ROTATE        1       // 0 = always landscape (buttons at bottom)
// Display rotation to use depending on which edge of the device points up.
// (Edges named as seen in normal landscape with buttons at the bottom.)
#define ROT_TOP_UP         1       // normal landscape
#define ROT_BOTTOM_UP      3       // upside-down landscape
#define ROT_RIGHT_UP       2       // portrait (buttons on the right)
#define ROT_LEFT_UP        0       // portrait (buttons on the left)
