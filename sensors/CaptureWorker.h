/*
 * SPDX-FileCopyrightText: 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <aidl/vendor/lineage/oplus_als/AreaRgbCaptureResult.h>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace android::hardware::sensors::V2_1::implementation {

class CaptureWorker {
  public:
    using Result = aidl::vendor::lineage::oplus_als::AreaRgbCaptureResult;

    explicit CaptureWorker(std::function<bool(Result*)> capture)
        : mState(std::make_shared<State>()) {
        // Binder calls cannot be cancelled. The worker owns its state even if
        // the client is destroyed while a transaction is still in flight.
        std::thread([state = mState, capture = std::move(capture)] {
            std::unique_lock lock(state->mutex);
            while (true) {
                state->changed.wait(lock, [&] { return state->pending || state->stopping; });
                if (state->stopping) return;
                state->pending = false;
                lock.unlock();
                Result result{};
                const bool success = capture(&result);
                lock.lock();
                state->result = result;
                state->success = success;
                state->busy = false;
                state->completed = true;
                state->changed.notify_all();
            }
        }).detach();
    }

    ~CaptureWorker() {
        std::lock_guard lock(mState->mutex);
        mState->stopping = true;
        mState->changed.notify_all();
    }

    CaptureWorker(const CaptureWorker&) = delete;
    CaptureWorker& operator=(const CaptureWorker&) = delete;

    bool capture(Result* result,
                 std::chrono::milliseconds timeout = std::chrono::milliseconds(100)) {
        std::unique_lock lock(mState->mutex);
        if (mState->busy) return false;
        mState->busy = true;
        mState->pending = true;
        mState->completed = false;
        mState->changed.notify_all();
        if (!mState->changed.wait_for(lock, timeout, [&] { return mState->completed; }) ||
            !mState->success) {
            return false;
        }
        *result = mState->result;
        return true;
    }

  private:
    struct State {
        std::mutex mutex;
        std::condition_variable changed;
        bool stopping = false;
        bool pending = false;
        bool busy = false;
        bool completed = false;
        bool success = false;
        Result result{};
    };
    std::shared_ptr<State> mState;
};

}  // namespace android::hardware::sensors::V2_1::implementation
