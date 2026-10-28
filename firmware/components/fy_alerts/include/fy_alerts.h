// SPDX-License-Identifier: MIT
//
// Detection alerts on every board: upstream flock-you's buzzer sounds on a
// passive piezo (FY_BUZZER_GPIO), and the owner's alert preferences, which
// the Heltec's screen flash follows too.

#pragma once

#include <cstdint>

#include "esp_err.h"
#include "fy_alerts_logic.h"

// Loads the preferences, starts taking sightings, and plays the start-up
// tune. Call after NVS is initialised and before fy_detector_start().
esp_err_t fy_alerts_start();

fy::alerts::Settings fy_alerts_settings();
// Applies new preferences and stores them; false when they could not be
// stored (they still apply until restart).
bool fy_alerts_set(const fy::alerts::Settings &settings);

// Plays the alert once with the current preferences: the most confident
// tier's sound, and on boards with a screen the screen flash, which watches
// fy_alerts_tests().
void fy_alerts_test();
uint32_t fy_alerts_tests();
