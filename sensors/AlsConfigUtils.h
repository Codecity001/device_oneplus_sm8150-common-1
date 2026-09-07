/*
 * Copyright (C) 2026 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cmath>
#include <cstddef>
#include <sstream>
#include <string>

namespace android::hardware::sensors::V2_1::implementation {

template <std::size_t N>
bool parseAlsValues(const std::string& text, float (&values)[N]) {
    std::istringstream stream(text);
    float parsed[N];
    for (float& value : parsed) {
        if (!(stream >> value) || !std::isfinite(value)) return false;
    }
    stream >> std::ws;
    if (!stream.eof()) return false;
    for (std::size_t i = 0; i < N; ++i) values[i] = parsed[i];
    return true;
}

inline bool positiveAlsValue(float value) {
    return std::isfinite(value) && value > 0.0f;
}

inline float alsCalibrationOr(float calibration, float fallback) {
    return positiveAlsValue(calibration) ? calibration : fallback;
}

}  // namespace android::hardware::sensors::V2_1::implementation
