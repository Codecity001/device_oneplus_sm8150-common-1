/*
 * Copyright (C) 2021-2024 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "AlsCorrection.h"
#include "AlsConfigUtils.h"
#include "CaptureWorker.h"

#include <android-base/properties.h>
#include <android/binder_manager.h>
#include <binder/IBinder.h>
#include <binder/IServiceManager.h>
#include <cmath>
#include <fstream>
#include <log/log.h>
#include <utils/Timers.h>

using aidl::vendor::lineage::oplus_als::AreaRgbCaptureResult;
using aidl::vendor::lineage::oplus_als::IAreaCapture;
using android::base::GetBoolProperty;
using android::base::GetIntProperty;
using android::base::GetProperty;

#define ALS_CALI_DIR "/proc/sensor/als_cali/"
#define BRIGHTNESS_DIR "/sys/class/backlight/panel0-backlight/"

namespace android {
namespace hardware {
namespace sensors {
namespace V2_1 {
namespace implementation {

static const std::string rgbw_max_lux_paths[4] = {
    ALS_CALI_DIR "red_max_lux",
    ALS_CALI_DIR "green_max_lux",
    ALS_CALI_DIR "blue_max_lux",
    ALS_CALI_DIR "white_max_lux",
};

struct als_config {
    bool hbr;
    float rgbw_max_lux[4];
    float rgbw_max_lux_div[4];
    float rgbw_lux_postmul[4];
    float rgbw_poly[4][4];
    float grayscale_weights[3];
    float sensor_gaincal_points[4];
    float sensor_inverse_gain[4];
    float agc_threshold;
    float calib_gain;
    float bias;
    float max_brightness;
};

static const struct {
    float middle;
    float min, max;
} hysteresis_ranges[] = {
    { 0, 0, 4 },
    { 7, 1, 12 },
    { 15, 5, 30 },
    { 30, 10, 50 },
    { 360, 25, 700 },
    { 1200, 300, 1600 },
    { 2250, 1000, 2940 },
    { 4600, 2000, 5900 },
    { 10000, 4000, 80000 },
    { HUGE_VALF, 8000, HUGE_VALF },
};

static struct {
    nsecs_t last_update, last_forced_update;
    bool force_update;
    float hyst_min, hyst_max;
    float last_corrected_value;
    float last_agc_gain;
} state = {
    .last_update = 0,
    .force_update = true,
    .hyst_min = -1.0, .hyst_max = -1.0,
    .last_agc_gain = 0.0,
};

static als_config conf;
static bool configValid = false;

class AreaCaptureConnection {
    std::shared_ptr<IAreaCapture> service;
    std::chrono::steady_clock::time_point nextLookup = std::chrono::steady_clock::time_point::min();

  public:
    bool operator()(AreaRgbCaptureResult* result) {
        if (service == nullptr) {
            const auto now = std::chrono::steady_clock::now();
            if (now < nextLookup) return false;
            nextLookup = now + std::chrono::seconds(1);
            const auto instance = std::string(IAreaCapture::descriptor) + "/default";
            service = IAreaCapture::fromBinder(
                    ::ndk::SpAIBinder(AServiceManager_checkService(instance.c_str())));
            if (service == nullptr) return false;
        }
        const auto status = service->getAreaBrightness(result);
        if (!status.isOk() && status.getStatus() != STATUS_OK) {
            service.reset();
        }
        return status.isOk();
    }
};

template <typename T>
static T get(const std::string& path, const T& def) {
    std::ifstream file(path);
    T result;

    file >> result;
    return file.fail() ? def : result;
}

void AlsCorrection::init() {
    configValid = false;
    conf = {};
    conf.hbr = GetBoolProperty("vendor.sensors.als_correction.hbr", false);
    conf.bias = GetIntProperty("vendor.sensors.als_correction.bias", 0);
    const auto readValues = [](const char* suffix, auto& values) {
        const std::string property = std::string("vendor.sensors.als_correction.") + suffix;
        if (parseAlsValues(GetProperty(property, ""), values)) return true;
        ALOGE("Invalid ALS configuration: %s", property.c_str());
        return false;
    };
    if (!readValues("rgbw_max_lux_div", conf.rgbw_max_lux_div)
            || !readValues("rgbw_poly1", conf.rgbw_poly[0])
            || !readValues("rgbw_poly2", conf.rgbw_poly[1])
            || !readValues("rgbw_poly3", conf.rgbw_poly[2])
            || !readValues("rgbw_poly4", conf.rgbw_poly[3])
            || !readValues("grayscale_weights", conf.grayscale_weights)
            || !readValues("sensor_gaincal_points", conf.sensor_gaincal_points)
            || !readValues("sensor_inverse_gain", conf.sensor_inverse_gain)) {
        return;
    }

    conf.sensor_inverse_gain[0] = alsCalibrationOr(
            get(ALS_CALI_DIR "row_coe", 0.0f) / 1000.0f, conf.sensor_inverse_gain[0]);

    // Factory calibration takes precedence; the property is a per-channel fallback.
    parseAlsValues(GetProperty("vendor.sensors.als_correction.rgbw_max_lux", ""),
                   conf.rgbw_max_lux);
    for (int i = 0; i < 4; ++i) {
        conf.rgbw_max_lux[i] = alsCalibrationOr(
                get(rgbw_max_lux_paths[i], 0.0f), conf.rgbw_max_lux[i]);
        if (!positiveAlsValue(conf.rgbw_max_lux[i])
                || !positiveAlsValue(conf.rgbw_max_lux_div[i])
                || !positiveAlsValue(conf.sensor_inverse_gain[i])
                || conf.sensor_gaincal_points[i] < 0.0f) {
            ALOGE("Invalid ALS calibration for channel %d; correction disabled", i);
            return;
        }
    }
    for (float weight : conf.grayscale_weights) {
        if (weight < 0.0f) {
            ALOGE("Invalid ALS grayscale weight; correction disabled");
            return;
        }
    }

    float rgbw_acc = 0.0;
    for (int i = 0; i < 4; i++) {
        if (i < 3) {
            rgbw_acc += conf.rgbw_max_lux[i];
            conf.rgbw_lux_postmul[i] = conf.rgbw_max_lux[i] / conf.rgbw_max_lux_div[i];
        } else {
            rgbw_acc -= conf.rgbw_max_lux[i];
            conf.rgbw_lux_postmul[i] = rgbw_acc / conf.rgbw_max_lux_div[i];
        }
        if (!std::isfinite(conf.rgbw_lux_postmul[i])) {
            ALOGE("Invalid ALS display scale for channel %d; correction disabled", i);
            return;
        }
    }
    ALOGI("Display maximums: R=%.0f G=%.0f B=%.0f W=%.0f",
        conf.rgbw_max_lux[0], conf.rgbw_max_lux[1],
        conf.rgbw_max_lux[2], conf.rgbw_max_lux[3]);

    conf.agc_threshold = 800.0 / conf.sensor_inverse_gain[0];

    conf.calib_gain = alsCalibrationOr(get(ALS_CALI_DIR "cali_coe", 0.0f) / 1000.0f, 1.0f);
    if (!positiveAlsValue(conf.calib_gain * conf.sensor_inverse_gain[0])
            || !positiveAlsValue(conf.agc_threshold)) {
        ALOGE("Invalid ALS gain scale; correction disabled");
        return;
    }
    ALOGI("Calibrated sensor gain: %.2fx", 1.0 / (conf.calib_gain * conf.sensor_inverse_gain[0]));

    conf.max_brightness = alsCalibrationOr(
            get(BRIGHTNESS_DIR "max_brightness", 1023.0f), 1023.0f);
    configValid = true;
}

void AlsCorrection::process(Event& event) {
    if (!configValid) return;

    static CaptureWorker captureWorker(AreaCaptureConnection{});
    AreaRgbCaptureResult screenshot{};

    ALOGV("Raw sensor reading: %.0f", event.u.scalar);

    if (event.u.scalar > conf.bias) {
        event.u.scalar -= conf.bias;
    }

    nsecs_t now = systemTime(SYSTEM_TIME_BOOTTIME);
    float brightness = get(BRIGHTNESS_DIR "brightness", 0.0);

    if (state.last_update == 0) {
        state.last_update = now;
        state.last_forced_update = now;
    } else {
        if (brightness > 0.0 && (now - state.last_forced_update) > s2ns(3)) {
            ALOGV("Forcing screenshot");
            state.last_forced_update = now;
            state.force_update = true;
        }
        if ((now - state.last_update) < ms2ns(100)) {
            ALOGV("Events coming too fast, dropping");
            // TODO figure out a better way to drop events
            event.sensorHandle = 0;
            return;
        }
        state.last_update = now;
    }

    float sensor_raw_calibrated = event.u.scalar * conf.calib_gain * state.last_agc_gain;
    if (state.force_update
            || ((event.u.scalar < state.hyst_min || event.u.scalar > state.hyst_max)
                && (sensor_raw_calibrated < 10.0 || sensor_raw_calibrated > (5.0 / .07)))) {

        if (!captureWorker.capture(&screenshot)) {
            ALOGE("Could not get area above sensor");
            // TODO figure out a better way to drop events
            event.sensorHandle = 0;
            return;
        }
        ALOGV("Screen color above sensor: %f %f %f", screenshot.r, screenshot.g, screenshot.b);

        float rgbw[4] = {
            screenshot.r, screenshot.g, screenshot.b,
            screenshot.r * conf.grayscale_weights[0]
                + screenshot.g * conf.grayscale_weights[1]
                + screenshot.b * conf.grayscale_weights[2]
        };
        float cumulative_correction = 0.0;
        for (int i = 0; i < 4; i++) {
            float corr = 0.0;
            for (float coef : conf.rgbw_poly[i]) {
                corr *= rgbw[i];
                corr += coef;
            }
            corr *= conf.rgbw_lux_postmul[i];
            if (i < 3) {
                cumulative_correction += std::max(corr, 0.0f);
            } else {
                cumulative_correction -= corr;
            }
        }
        cumulative_correction *= brightness / conf.max_brightness;
        float brightness_fullwhite = conf.rgbw_max_lux[3] * brightness / conf.max_brightness;
        float brightness_grayscale_gamma = std::pow(rgbw[3] / 255.0, 2.2) * brightness_fullwhite;
        cumulative_correction = std::min(cumulative_correction, brightness_fullwhite);
        cumulative_correction = std::max(cumulative_correction, brightness_grayscale_gamma);
        ALOGV("Estimated screen brightness: %.0f", cumulative_correction);

        float sensor_raw_corrected = std::max(event.u.scalar - cumulative_correction, 0.0f);

        float agc_gain = conf.sensor_inverse_gain[0];
        if (sensor_raw_corrected > conf.agc_threshold) {
            float gain_estimate = 0;
            if (conf.hbr) {
                gain_estimate = event.u.data[2] * 1000.0 / sensor_raw_corrected;
            } else {
                gain_estimate = sensor_raw_corrected / event.u.data[2];
            }
            for (int i = 0; i < 4; i++) {
                if (gain_estimate > conf.sensor_gaincal_points[i]) {
                    agc_gain = conf.sensor_inverse_gain[i];
                }
            }
        }
        ALOGV("AGC gain: %f", agc_gain);

        if (cumulative_correction <= event.u.scalar * 1.35
                || event.u.scalar * conf.calib_gain * agc_gain < 10000.0
                || state.force_update) {
            float sensor_corrected = sensor_raw_corrected * conf.calib_gain * agc_gain;
            state.last_agc_gain = agc_gain;
            for (auto& range : hysteresis_ranges) {
                if (sensor_corrected <= range.middle) {
                    state.hyst_min = range.middle == 0.0f ? -1.0f
                            : range.min / (conf.calib_gain * conf.sensor_inverse_gain[0]);
                    state.hyst_max = range.max / (conf.calib_gain * conf.sensor_inverse_gain[0])
                            + brightness_fullwhite;
                    break;
                }
            }
            sensor_corrected = std::max(sensor_corrected - 14.0, 0.0);
            event.u.scalar = sensor_corrected;
            state.last_corrected_value = sensor_corrected;
            ALOGV("Fully corrected sensor value: %.0f lux", sensor_corrected);
        } else {
            event.u.scalar = state.last_corrected_value;
            ALOGV("Reusing cached value: %.0f lux", event.u.scalar);
        }

        state.force_update = false;
    } else {
        event.u.scalar = state.last_corrected_value;
        ALOGV("Reusing cached value: %.0f lux", event.u.scalar);
    }
}

}  // namespace implementation
}  // namespace V2_1
}  // namespace sensors
}  // namespace hardware
}  // namespace android
