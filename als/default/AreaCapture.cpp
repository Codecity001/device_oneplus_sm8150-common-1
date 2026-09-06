/*
 * Copyright (C) 2021-2024 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "AreaCapture.h"

#include <android-base/properties.h>
#include <android/gui/BnScreenCaptureListener.h>
#include <gui/AidlUtil.h>
#include <gui/SurfaceComposerClient.h>
#include <ui/DisplayState.h>
#include <ui/PixelFormat.h>
#include <chrono>
#include <future>

using ::android::DisplayCaptureArgs;
using ::android::GraphicBuffer;
using ::android::IBinder;
using ::android::Rect;
using ::android::ScreenshotClient;
using ::android::sp;
using ::android::SurfaceComposerClient;

using ::android::base::GetProperty;
using ::android::gui::ScreenCaptureResults;
using ::android::gui::SecureLayerMode;
using ::android::gui::aidl_utils::toARect;
using ::android::ui::PixelFormat;

namespace aidl {
namespace vendor {
namespace lineage {
namespace oplus_als {

namespace {

class CaptureListener : public ::android::gui::BnScreenCaptureListener {
  public:
    ::android::binder::Status onScreenCaptureCompleted(const ScreenCaptureResults& result) override {
        mResult.set_value(result);
        return ::android::binder::Status::ok();
    }

    bool waitForResults(ScreenCaptureResults* result) {
        auto future = mResult.get_future();
        if (future.wait_for(std::chrono::milliseconds(500)) != std::future_status::ready) {
            return false;
        }
        *result = future.get();
        return result->fenceResult.ok() &&
               result->fenceResult.value()->wait(500) == ::android::NO_ERROR;
    }

  private:
    std::promise<ScreenCaptureResults> mResult;
};

}  // namespace

AreaCapture::AreaCapture() {
    int left = 0, top = 0, right = 0, bottom = 0;
    std::istringstream is(GetProperty("vendor.sensors.als_correction.grabrect", ""));

    if (!(is >> left >> top >> right >> bottom) || left < 0 || top < 0 || right <= left ||
        bottom <= top) {
        ALOGE("Invalid screenshot grab area config");
        return;
    }

    ALOGI("Screenshot grab area: %d %d %d %d", left, top, right, bottom);
    m_screenshot_rect = Rect(left, top, right, bottom);
}

// See frameworks/base/services/core/jni/com_android_server_display_DisplayControl.cpp and
// frameworks/base/core/java/android/view/SurfaceControl.java
sp<IBinder> AreaCapture::getInternalDisplayToken() {
    const auto displayIds = SurfaceComposerClient::getPhysicalDisplayIds();
    if (displayIds.empty()) return nullptr;
    return SurfaceComposerClient::getPhysicalDisplayToken(displayIds[0]);
}

ndk::ScopedAStatus AreaCapture::getAreaBrightness(AreaRgbCaptureResult* _aidl_return) {
    if (m_screenshot_rect.isEmpty()) {
        return ndk::ScopedAStatus::fromServiceSpecificError(::android::BAD_VALUE);
    }
    DisplayCaptureArgs displayCaptureArgs;
    displayCaptureArgs.displayToken = getInternalDisplayToken();
    if (displayCaptureArgs.displayToken == nullptr) {
        return ndk::ScopedAStatus::fromServiceSpecificError(::android::NAME_NOT_FOUND);
    }
    displayCaptureArgs.captureArgs.pixelFormat = ::android::PIXEL_FORMAT_RGBA_8888;
    displayCaptureArgs.captureArgs.sourceCrop = toARect(m_screenshot_rect);
    displayCaptureArgs.width = m_screenshot_rect.getWidth();
    displayCaptureArgs.height = m_screenshot_rect.getHeight();
    displayCaptureArgs.captureArgs.secureLayerMode = SecureLayerMode::Capture;

    sp<CaptureListener> captureListener = new CaptureListener();
    if (ScreenshotClient::captureDisplay(displayCaptureArgs, captureListener) !=
        ::android::NO_ERROR) {
        ALOGE("Capture failed");
        return ndk::ScopedAStatus::fromServiceSpecificError(-1);
    }

    ScreenCaptureResults captureResults;
    if (!captureListener->waitForResults(&captureResults) || captureResults.buffer == nullptr) {
        ALOGE("Capture result unavailable or fence wait failed");
        return ndk::ScopedAStatus::fromServiceSpecificError(-1);
    }

    auto resultWidth = captureResults.buffer->getWidth();
    auto resultHeight = captureResults.buffer->getHeight();
    auto stride = captureResults.buffer->getStride();
    if (resultWidth == 0 || resultHeight == 0 || stride < resultWidth ||
        captureResults.buffer->getPixelFormat() != ::android::PIXEL_FORMAT_RGBA_8888) {
        return ndk::ScopedAStatus::fromServiceSpecificError(::android::BAD_VALUE);
    }
    uint8_t* out = nullptr;
    const auto status = captureResults.buffer->lock(GraphicBuffer::USAGE_SW_READ_OFTEN,
                                                    reinterpret_cast<void**>(&out));
    if (status != ::android::NO_ERROR) {
        return ndk::ScopedAStatus::fromServiceSpecificError(status);
    }
    if (out == nullptr) {
        captureResults.buffer->unlock();
        return ndk::ScopedAStatus::fromServiceSpecificError(::android::BAD_VALUE);
    }

    // we can sum this directly on linear light
    uint64_t rsum = 0, gsum = 0, bsum = 0;
    for (int y = 0; y < resultHeight; y++) {
        for (int x = 0; x < resultWidth; x++) {
            rsum += out[y * (stride * 4) + x * 4];
            gsum += out[y * (stride * 4) + x * 4 + 1];
            bsum += out[y * (stride * 4) + x * 4 + 2];
        }
    }

    float max = static_cast<float>(resultWidth) * resultHeight;
    _aidl_return->r = rsum / max;
    _aidl_return->g = gsum / max;
    _aidl_return->b = bsum / max;

    captureResults.buffer->unlock();

    return ndk::ScopedAStatus::ok();
}

}  // namespace oplus_als
}  // namespace lineage
}  // namespace vendor
}  // namespace aidl
