/*
 * Copyright (c) 2019 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

package org.lineageos.camerahelper;

import android.app.AlertDialog;
import android.content.Context;
import android.content.Intent;
import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
import android.hardware.SensorManager;
import android.os.Handler;
import android.os.Looper;
import android.text.TextUtils;
import android.util.Log;
import android.view.ContextThemeWrapper;
import android.view.WindowManager;

import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public class FallSensor implements SensorEventListener {
    private static final boolean DEBUG = true;
    private static final String TAG = "FallSensor";

    private ExecutorService mExecutorService;
    private SensorManager mSensorManager;
    private Sensor mSensor;
    private Context mContext;
    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private boolean mClosed;
    private AlertDialog mAlertDialog;

    public FallSensor(Context context) {
        mContext = context;
        mSensorManager = mContext.getSystemService(SensorManager.class);
        mExecutorService = Executors.newSingleThreadExecutor();

        for (Sensor sensor : mSensorManager.getSensorList(Sensor.TYPE_ALL)) {
            if (DEBUG) Log.d(TAG, "Sensor type: " + sensor.getStringType());
            if (TextUtils.equals(sensor.getStringType(), "camera_protect")) {
                if (DEBUG) Log.d(TAG, "Found fall sensor");
                mSensor = sensor;
                break;
            }
        }
    }

    @Override
    public void onSensorChanged(SensorEvent event) {
        if (mClosed || event.values[0] <= 0) {
            return;
        }

        Log.d(TAG, "Fall detected, ensuring front camera is closed");

        // We shouldn't really bother doing anything if motor is already closed
        if (CameraMotorController.POSITION_DOWN.equals(CameraMotorController.getMotorPosition())) {
            return;
        }

        // Close the camera
        CameraMotorController.setMotorDirection(CameraMotorController.DIRECTION_DOWN);
        CameraMotorController.setMotorEnabled();

        // Show alert dialog informing user that we closed the camera
        mHandler.post(() -> {
            if (mClosed) return;
            if (mAlertDialog != null) mAlertDialog.dismiss();
            Context context = new ContextThemeWrapper(
                    mContext, R.style.Theme_SubSettingsBase_Expressive);
            AlertDialog alertDialog = new AlertDialog.Builder(context)
                    .setTitle(R.string.free_fall_detected_title)
                    .setMessage(R.string.free_fall_detected_message)
                    .setNegativeButton(R.string.raise_the_camera, (dialog, which) -> {
                        // Reopen the camera
                        CameraMotorController.setMotorDirection(CameraMotorController.DIRECTION_UP);
                        CameraMotorController.setMotorEnabled();
                    })
                    .setPositiveButton(R.string.close, (dialog, which) -> {
                        // Go back to home screen
                        Intent intent = new Intent(Intent.ACTION_MAIN);
                        intent.addCategory(Intent.CATEGORY_HOME);
                        intent.setFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                        mContext.startActivity(intent);
                    })
                    .create();
            alertDialog.getWindow().setType(WindowManager.LayoutParams.TYPE_SYSTEM_ALERT);
            alertDialog.setCanceledOnTouchOutside(false);
            mAlertDialog = alertDialog;
            alertDialog.show();
        });
    }

    @Override
    public void onAccuracyChanged(Sensor sensor, int accuracy) {
        /* Empty */
    }

    void enable() {
        if (mClosed || mSensor == null) return;
        if (DEBUG) Log.d(TAG, "Enabling");
        mExecutorService.submit(() -> {
            mSensorManager.registerListener(this, mSensor, SensorManager.SENSOR_DELAY_FASTEST, mHandler);
        });
    }

    void disable() {
        if (mClosed || mSensor == null) return;
        if (DEBUG) Log.d(TAG, "Disabling");
        mExecutorService.submit(() -> {
            mSensorManager.unregisterListener(this, mSensor);
        });
    }
    void close() {
        if (mClosed) return;
        disable();
        mClosed = true;
        mExecutorService.shutdown();
        mHandler.removeCallbacksAndMessages(null);
        if (mAlertDialog != null) {
            mAlertDialog.dismiss();
            mAlertDialog = null;
        }
    }
}
